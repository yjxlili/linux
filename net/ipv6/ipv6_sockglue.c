// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	IPv6 BSD socket options interface
 *	Linux INET6 implementation
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *
 *	Based on linux/net/ipv4/ip_sockglue.c
 *
 *	FIXME: Make the setsockopt code POSIX compliant: That is
 *
 *	o	Truncate getsockopt returns
 *	o	Return an optlen of the truncated length if need be
 *
 *	Changes:
 *	David L Stevens <dlstevens@us.ibm.com>:
 *		- added multicast source filtering API for MLDv2
 */

#include <linux/module.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <linux/mroute6.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/netfilter.h>
#include <linux/slab.h>

#include <net/sock.h>
#include <net/snmp.h>
#include <net/ipv6.h>
#include <net/ndisc.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/inet_common.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/udplite.h>
#include <net/xfrm.h>
#include <net/compat.h>
#include <net/seg6.h>

#include <linux/uaccess.h>

struct ip6_ra_chain *ip6_ra_chain;
DEFINE_RWLOCK(ip6_ra_lock);

DEFINE_STATIC_KEY_FALSE(ip6_min_hopcount);

int ip6_ra_control(struct sock *sk, int sel)
{
	struct ip6_ra_chain *ra, *new_ra, **rap;

	/* RA packet may be delivered ONLY to IPPROTO_RAW socket */
	if (sk->sk_type != SOCK_RAW || inet_sk(sk)->inet_num != IPPROTO_RAW)
		return -ENOPROTOOPT;

	new_ra = (sel >= 0) ? kmalloc(sizeof(*new_ra), GFP_KERNEL) : NULL;
	if (sel >= 0 && !new_ra)
		return -ENOMEM;

	write_lock_bh(&ip6_ra_lock);
	for (rap = &ip6_ra_chain; (ra = *rap) != NULL; rap = &ra->next) {
		if (ra->sk == sk) {
			if (sel >= 0) {
				write_unlock_bh(&ip6_ra_lock);
				kfree(new_ra);
				return -EADDRINUSE;
			}

			*rap = ra->next;
			write_unlock_bh(&ip6_ra_lock);

			sock_put(sk);
			kfree(ra);
			return 0;
		}
	}
	if (!new_ra) {
		write_unlock_bh(&ip6_ra_lock);
		return -ENOBUFS;
	}
	new_ra->sk = sk;
	new_ra->sel = sel;
	new_ra->next = ra;
	*rap = new_ra;
	sock_hold(sk);
	write_unlock_bh(&ip6_ra_lock);
	return 0;
}

struct ipv6_txoptions *ipv6_update_options(struct sock *sk,
					   struct ipv6_txoptions *opt)
{
	if (inet_test_bit(IS_ICSK, sk)) {
		if (opt &&
		    !((1 << sk->sk_state) & (TCPF_LISTEN | TCPF_CLOSE)) &&
		    inet_sk(sk)->inet_daddr != LOOPBACK4_IPV6) {
			struct inet_connection_sock *icsk = inet_csk(sk);
			icsk->icsk_ext_hdr_len = opt->opt_flen + opt->opt_nflen;
			icsk->icsk_sync_mss(sk, icsk->icsk_pmtu_cookie);
		}
	}
	opt = unrcu_pointer(xchg(&inet6_sk(sk)->opt, RCU_INITIALIZER(opt)));
	sk_dst_reset(sk);

	return opt;
}

static int copy_group_source_from_sockptr(struct group_source_req *greqs,
		sockptr_t optval, int optlen)
{
	if (in_compat_syscall()) {
		struct compat_group_source_req gr32;

		if (optlen < sizeof(gr32))
			return -EINVAL;
		if (copy_from_sockptr(&gr32, optval, sizeof(gr32)))
			return -EFAULT;
		greqs->gsr_interface = gr32.gsr_interface;
		greqs->gsr_group = gr32.gsr_group;
		greqs->gsr_source = gr32.gsr_source;
	} else {
		if (optlen < sizeof(*greqs))
			return -EINVAL;
		if (copy_from_sockptr(greqs, optval, sizeof(*greqs)))
			return -EFAULT;
	}

	return 0;
}

static int do_ipv6_mcast_group_source(struct sock *sk, int optname,
		sockptr_t optval, int optlen)
{
	struct group_source_req greqs;
	int omode, add;
	int ret;

	ret = copy_group_source_from_sockptr(&greqs, optval, optlen);
	if (ret)
		return ret;

	if (greqs.gsr_group.ss_family != AF_INET6 ||
	    greqs.gsr_source.ss_family != AF_INET6)
		return -EADDRNOTAVAIL;

	if (optname == MCAST_BLOCK_SOURCE) {
		omode = MCAST_EXCLUDE;
		add = 1;
	} else if (optname == MCAST_UNBLOCK_SOURCE) {
		omode = MCAST_EXCLUDE;
		add = 0;
	} else if (optname == MCAST_JOIN_SOURCE_GROUP) {
		struct sockaddr_in6 *psin6;
		int retv;

		psin6 = (struct sockaddr_in6 *)&greqs.gsr_group;
		retv = ipv6_sock_mc_join_ssm(sk, greqs.gsr_interface,
					     &psin6->sin6_addr,
					     MCAST_INCLUDE);
		/* prior join w/ different source is ok */
		if (retv && retv != -EADDRINUSE)
			return retv;
		omode = MCAST_INCLUDE;
		add = 1;
	} else /* MCAST_LEAVE_SOURCE_GROUP */ {
		omode = MCAST_INCLUDE;
		add = 0;
	}
	return ip6_mc_source(add, omode, sk, &greqs);
}

static int ipv6_set_mcast_msfilter(struct sock *sk, sockptr_t optval,
		int optlen)
{
	struct group_filter *gsf;
	int ret;

	if (optlen < GROUP_FILTER_SIZE(0))
		return -EINVAL;
	if (optlen > READ_ONCE(sock_net(sk)->core.sysctl_optmem_max))
		return -ENOBUFS;

	gsf = memdup_sockptr(optval, optlen);
	if (IS_ERR(gsf))
		return PTR_ERR(gsf);

	/* numsrc >= (4G-140)/128 overflow in 32 bits */
	ret = -ENOBUFS;
	if (gsf->gf_numsrc >= 0x1ffffffU ||
	    gsf->gf_numsrc > sysctl_mld_max_msf)
		goto out_free_gsf;

	ret = -EINVAL;
	if (GROUP_FILTER_SIZE(gsf->gf_numsrc) > optlen)
		goto out_free_gsf;

	ret = ip6_mc_msfilter(sk, gsf, gsf->gf_slist_flex);
out_free_gsf:
	kfree(gsf);
	return ret;
}

static int compat_ipv6_set_mcast_msfilter(struct sock *sk, sockptr_t optval,
		int optlen)
{
	const int size0 = offsetof(struct compat_group_filter, gf_slist_flex);
	struct compat_group_filter *gf32;
	void *p;
	int ret;
	int n;

	if (optlen < size0)
		return -EINVAL;
	if (optlen > READ_ONCE(sock_net(sk)->core.sysctl_optmem_max) - 4)
		return -ENOBUFS;

	p = kmalloc(optlen + 4, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	gf32 = p + 4; /* we want ->gf_group and ->gf_slist_flex aligned */
	ret = -EFAULT;
	if (copy_from_sockptr(gf32, optval, optlen))
		goto out_free_p;

	/* numsrc >= (4G-140)/128 overflow in 32 bits */
	ret = -ENOBUFS;
	n = gf32->gf_numsrc;
	if (n >= 0x1ffffffU || n > sysctl_mld_max_msf)
		goto out_free_p;

	ret = -EINVAL;
	if (offsetof(struct compat_group_filter, gf_slist_flex[n]) > optlen)
		goto out_free_p;

	ret = ip6_mc_msfilter(sk, &(struct group_filter){
			.gf_interface = gf32->gf_interface,
			.gf_group = gf32->gf_group,
			.gf_fmode = gf32->gf_fmode,
			.gf_numsrc = gf32->gf_numsrc}, gf32->gf_slist_flex);

out_free_p:
	kfree(p);
	return ret;
}

static int ipv6_mcast_join_leave(struct sock *sk, int optname,
		sockptr_t optval, int optlen)
{
	struct sockaddr_in6 *psin6;
	struct group_req greq;

	if (optlen < sizeof(greq))
		return -EINVAL;
	if (copy_from_sockptr(&greq, optval, sizeof(greq)))
		return -EFAULT;

	if (greq.gr_group.ss_family != AF_INET6)
		return -EADDRNOTAVAIL;
	psin6 = (struct sockaddr_in6 *)&greq.gr_group;
	if (optname == MCAST_JOIN_GROUP)
		return ipv6_sock_mc_join(sk, greq.gr_interface,
					 &psin6->sin6_addr);
	return ipv6_sock_mc_drop(sk, greq.gr_interface, &psin6->sin6_addr);
}

static int compat_ipv6_mcast_join_leave(struct sock *sk, int optname,
		sockptr_t optval, int optlen)
{
	struct compat_group_req gr32;
	struct sockaddr_in6 *psin6;

	if (optlen < sizeof(gr32))
		return -EINVAL;
	if (copy_from_sockptr(&gr32, optval, sizeof(gr32)))
		return -EFAULT;

	if (gr32.gr_group.ss_family != AF_INET6)
		return -EADDRNOTAVAIL;
	psin6 = (struct sockaddr_in6 *)&gr32.gr_group;
	if (optname == MCAST_JOIN_GROUP)
		return ipv6_sock_mc_join(sk, gr32.gr_interface,
					&psin6->sin6_addr);
	return ipv6_sock_mc_drop(sk, gr32.gr_interface, &psin6->sin6_addr);
}

static int ipv6_set_opt_hdr(struct sock *sk, int optname, sockptr_t optval,
		int optlen)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct ipv6_opt_hdr *new = NULL;
	struct net *net = sock_net(sk);
	struct ipv6_txoptions *opt;
	int err;

	/* hop-by-hop / destination options are privileged option */
	if (optname != IPV6_RTHDR && !sockopt_ns_capable(net->user_ns, CAP_NET_RAW))
		return -EPERM;

	/* remove any sticky options header with a zero option
	 * length, per RFC3542.
	 */
	if (optlen > 0) {
		if (sockptr_is_null(optval))
			return -EINVAL;
		if (optlen < sizeof(struct ipv6_opt_hdr) ||
		    optlen & 0x7 ||
		    optlen > 8 * 255)
			return -EINVAL;

		new = memdup_sockptr(optval, optlen);
		if (IS_ERR(new))
			return PTR_ERR(new);
		if (unlikely(ipv6_optlen(new) > optlen)) {
			kfree(new);
			return -EINVAL;
		}
	}

	opt = rcu_dereference_protected(np->opt, lockdep_sock_is_held(sk));
	opt = ipv6_renew_options(sk, opt, optname, new);
	kfree(new);
	if (IS_ERR(opt))
		return PTR_ERR(opt);

	/* routing header option needs extra check */
	err = -EINVAL;
	if (optname == IPV6_RTHDR && opt && opt->srcrt) {
		struct ipv6_rt_hdr *rthdr = opt->srcrt;
		switch (rthdr->type) {
#if IS_ENABLED(CONFIG_IPV6_MIP6)
		case IPV6_SRCRT_TYPE_2:
			if (rthdr->hdrlen != 2 || rthdr->segments_left != 1)
				goto sticky_done;
			break;
#endif
		case IPV6_SRCRT_TYPE_4:
		{
			struct ipv6_sr_hdr *srh =
				(struct ipv6_sr_hdr *)opt->srcrt;

			if (!seg6_validate_srh(srh, optlen, false))
				goto sticky_done;
			break;
		}
		default:
			goto sticky_done;
		}
	}

	err = 0;
	opt = ipv6_update_options(sk, opt);
sticky_done:
	if (opt) {
		atomic_sub(opt->tot_len, &sk->sk_omem_alloc);
		txopt_put(opt);
	}
	return err;
}

int do_ipv6_setsockopt(struct sock *sk, int level, int optname,
		       sockptr_t optval, unsigned int optlen)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct net *net = sock_net(sk);
	int retv = -ENOPROTOOPT;
	int val, valbool;

	if (sockptr_is_null(optval))
		val = 0;
	else {
		if (optlen >= sizeof(int)) {
			if (copy_from_sockptr(&val, optval, sizeof(val)))
				return -EFAULT;
		} else
			val = 0;
	}

	valbool = (val != 0);

	if (ip6_mroute_opt(optname))
		return ip6_mroute_setsockopt(sk, optname, optval, optlen);

	/* Handle options that can be set without locking the socket. */
	switch (optname) {
	case IPV6_UNICAST_HOPS:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val > 255 || val < -1)
			return -EINVAL;
		WRITE_ONCE(np->hop_limit, val);
		return 0;
	case IPV6_MULTICAST_LOOP:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val != valbool)
			return -EINVAL;
		inet6_assign_bit(MC6_LOOP, sk, valbool);
		return 0;
	case IPV6_MULTICAST_HOPS:
		if (sk->sk_type == SOCK_STREAM)
			return retv;
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val > 255 || val < -1)
			return -EINVAL;
		WRITE_ONCE(np->mcast_hops,
			   val == -1 ? IPV6_DEFAULT_MCASTHOPS : val);
		return 0;
	case IPV6_MTU:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val && val < IPV6_MIN_MTU)
			return -EINVAL;
		WRITE_ONCE(np->frag_size, val);
		return 0;
	case IPV6_MINHOPCOUNT:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val < 0 || val > 255)
			return -EINVAL;

		if (val)
			static_branch_enable(&ip6_min_hopcount);

		/* tcp_v6_err() and tcp_v6_rcv() might read min_hopcount
		 * while we are changing it.
		 */
		WRITE_ONCE(np->min_hopcount, val);
		return 0;
	case IPV6_RECVERR_RFC4884:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val < 0 || val > 1)
			return -EINVAL;
		inet6_assign_bit(RECVERR6_RFC4884, sk, valbool);
		return 0;
	case IPV6_MULTICAST_ALL:
		if (optlen < sizeof(int))
			return -EINVAL;
		inet6_assign_bit(MC6_ALL, sk, valbool);
		return 0;
	case IPV6_AUTOFLOWLABEL:
		inet6_assign_bit(AUTOFLOWLABEL, sk, valbool);
		inet6_set_bit(AUTOFLOWLABEL_SET, sk);
		return 0;
	case IPV6_DONTFRAG:
		inet6_assign_bit(DONTFRAG, sk, valbool);
		return 0;
	case IPV6_RECVERR:
		if (optlen < sizeof(int))
			return -EINVAL;
		inet6_assign_bit(RECVERR6, sk, valbool);
		if (!val)
			skb_errqueue_purge(&sk->sk_error_queue);
		return 0;
	case IPV6_ROUTER_ALERT_ISOLATE:
		if (optlen < sizeof(int))
			return -EINVAL;
		inet6_assign_bit(RTALERT_ISOLATE, sk, valbool);
		return 0;
	case IPV6_MTU_DISCOVER:
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val < IPV6_PMTUDISC_DONT || val > IPV6_PMTUDISC_OMIT)
			return -EINVAL;
		WRITE_ONCE(np->pmtudisc, val);
		return 0;
	case IPV6_FLOWINFO_SEND:
		if (optlen < sizeof(int))
			return -EINVAL;
		inet6_assign_bit(SNDFLOW, sk, valbool);
		return 0;
	case IPV6_ADDR_PREFERENCES:
		if (optlen < sizeof(int))
			return -EINVAL;
		return ip6_sock_set_addr_preferences(sk, val);
	case IPV6_MULTICAST_IF:
		if (sk->sk_type == SOCK_STREAM)
			return -ENOPROTOOPT;
		if (optlen < sizeof(int))
			return -EINVAL;
		if (val) {
			struct net_device *dev;
			int bound_dev_if, midx;

			rcu_read_lock();

			dev = dev_get_by_index_rcu(net, val);
			if (!dev) {
				rcu_read_unlock();
				return -ENODEV;
			}
			midx = l3mdev_master_ifindex_rcu(dev);

			rcu_read_unlock();

			bound_dev_if = READ_ONCE(sk->sk_bound_dev_if);
			if (bound_dev_if &&
			    bound_dev_if != val &&
			    (!midx || midx != bound_dev_if))
				return -EINVAL;
		}
		WRITE_ONCE(np->mcast_oif, val);
		return 0;
	case IPV6_UNICAST_IF:
	{
		struct net_device *dev;
		int ifindex;

		if (optlen != sizeof(int))
			return -EINVAL;

		ifindex = (__force int)ntohl((__force __be32)val);
		if (!ifindex) {
			WRITE_ONCE(np->ucast_oif, 0);
			return 0;
		}

		dev = dev_get_by_index(net, ifindex);
		if (!dev)
			return -EADDRNOTAVAIL;
		dev_put(dev);

		if (READ_ONCE(sk->sk_bound_dev_if))
			return -EINVAL;

		WRITE_ONCE(np->ucast_oif, ifindex);
		return 0;
	}
	}

	sockopt_lock_sock(sk);

	/* Another thread has converted the socket into IPv4 with
	 * IPV6_ADDRFORM concurrently.
	 */
	if (unlikely(sk->sk_family != AF_INET6))
		goto unlock;

	switch (optname) {

	case IPV6_ADDRFORM:
		if (optlen < sizeof(int))
			goto e_inval;
		if (val == PF_INET) {
			if (sk->sk_type == SOCK_RAW)
				break;

			if (sk->sk_protocol == IPPROTO_UDP ||
			    sk->sk_protocol == IPPROTO_UDPLITE) {
				struct udp_sock *up = udp_sk(sk);
				if (up->pending == AF_INET6) {
					retv = -EBUSY;
					break;
				}
			} else if (sk->sk_protocol == IPPROTO_TCP) {
				if (sk->sk_prot != &tcpv6_prot) {
					retv = -EBUSY;
					break;
				}
			} else {
				break;
			}

			if (sk->sk_state != TCP_ESTABLISHED) {
				retv = -ENOTCONN;
				break;
			}

			if (ipv6_only_sock(sk) ||
			    !ipv6_addr_v4mapped(&sk->sk_v6_daddr)) {
				retv = -EADDRNOTAVAIL;
				break;
			}

			__ipv6_sock_mc_close(sk);
			__ipv6_sock_ac_close(sk);

			if (sk->sk_protocol == IPPROTO_TCP) {
				struct inet_connection_sock *icsk = inet_csk(sk);

				sock_prot_inuse_add(net, sk->sk_prot, -1);
				sock_prot_inuse_add(net, &tcp_prot, 1);

				/* Paired with READ_ONCE(sk->sk_prot) in inet6_stream_ops */
				WRITE_ONCE(sk->sk_prot, &tcp_prot);
				/* Paired with READ_ONCE() in tcp_(get|set)sockopt() */
				WRITE_ONCE(icsk->icsk_af_ops, &ipv4_specific);
				WRITE_ONCE(sk->sk_socket->ops, &inet_stream_ops);
				WRITE_ONCE(sk->sk_family, PF_INET);
				tcp_sync_mss(sk, icsk->icsk_pmtu_cookie);
			} else {
				struct proto *prot = &udp_prot;

				if (sk->sk_protocol == IPPROTO_UDPLITE)
					prot = &udplite_prot;

				sock_prot_inuse_add(net, sk->sk_prot, -1);
				sock_prot_inuse_add(net, prot, 1);

				/* Paired with READ_ONCE(sk->sk_prot) in inet6_dgram_ops */
				WRITE_ONCE(sk->sk_prot, prot);
				WRITE_ONCE(sk->sk_socket->ops, &inet_dgram_ops);
				WRITE_ONCE(sk->sk_family, PF_INET);
			}

			/* Disable all options not to allocate memory anymore,
			 * but there is still a race.  See the lockless path
			 * in udpv6_sendmsg() and ipv6_local_rxpmtu().
			 */
			np->rxopt.all = 0;

			inet6_cleanup_sock(sk);

			module_put(THIS_MODULE);
			retv = 0;
			break;
		}
		goto e_inval;

	case IPV6_V6ONLY:
		if (optlen < sizeof(int) ||
		    inet_sk(sk)->inet_num)
			goto e_inval;
		sk->sk_ipv6only = valbool;
		retv = 0;
		break;

	case IPV6_RECVPKTINFO:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxinfo = valbool;
		retv = 0;
		break;

	case IPV6_2292PKTINFO:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxoinfo = valbool;
		retv = 0;
		break;

	case IPV6_RECVHOPLIMIT:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxhlim = valbool;
		retv = 0;
		break;

	case IPV6_2292HOPLIMIT:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxohlim = valbool;
		retv = 0;
		break;

	case IPV6_RECVRTHDR:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.srcrt = valbool;
		retv = 0;
		break;

	case IPV6_2292RTHDR:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.osrcrt = valbool;
		retv = 0;
		break;

	case IPV6_RECVHOPOPTS:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.hopopts = valbool;
		retv = 0;
		break;

	case IPV6_2292HOPOPTS:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.ohopopts = valbool;
		retv = 0;
		break;

	case IPV6_RECVDSTOPTS:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.dstopts = valbool;
		retv = 0;
		break;

	case IPV6_2292DSTOPTS:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.odstopts = valbool;
		retv = 0;
		break;

	case IPV6_TCLASS:
		if (optlen < sizeof(int))
			goto e_inval;
		if (val < -1 || val > 0xff)
			goto e_inval;
		/* RFC 3542, 6.5: default traffic class of 0x0 */
		if (val == -1)
			val = 0;
		if (sk->sk_type == SOCK_STREAM) {
			val &= ~INET_ECN_MASK;
			val |= np->tclass & INET_ECN_MASK;
		}
		if (np->tclass != val) {
			np->tclass = val;
			sk_dst_reset(sk);
		}
		retv = 0;
		break;

	case IPV6_RECVTCLASS:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxtclass = valbool;
		retv = 0;
		break;

	case IPV6_FLOWINFO:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxflow = valbool;
		retv = 0;
		break;

	case IPV6_RECVPATHMTU:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxpmtu = valbool;
		retv = 0;
		break;

	case IPV6_TRANSPARENT:
		if (valbool && !sockopt_ns_capable(net->user_ns, CAP_NET_RAW) &&
		    !sockopt_ns_capable(net->user_ns, CAP_NET_ADMIN)) {
			retv = -EPERM;
			break;
		}
		if (optlen < sizeof(int))
			goto e_inval;
		/* we don't have a separate transparent bit for IPV6 we use the one in the IPv4 socket */
		inet_assign_bit(TRANSPARENT, sk, valbool);
		retv = 0;
		break;

	case IPV6_FREEBIND:
		if (optlen < sizeof(int))
			goto e_inval;
		/* we also don't have a separate freebind bit for IPV6 */
		inet_assign_bit(FREEBIND, sk, valbool);
		retv = 0;
		break;

	case IPV6_RECVORIGDSTADDR:
		if (optlen < sizeof(int))
			goto e_inval;
		np->rxopt.bits.rxorigdstaddr = valbool;
		retv = 0;
		break;

	case IPV6_HOPOPTS:
	case IPV6_RTHDRDSTOPTS:
	case IPV6_RTHDR:
	case IPV6_DSTOPTS:
		retv = ipv6_set_opt_hdr(sk, optname, optval, optlen);
		break;

	case IPV6_PKTINFO:
	{
		struct in6_pktinfo pkt;

		if (optlen == 0)
			goto e_inval;
		else if (optlen < sizeof(struct in6_pktinfo) ||
			 sockptr_is_null(optval))
			goto e_inval;

		if (copy_from_sockptr(&pkt, optval, sizeof(pkt))) {
			retv = -EFAULT;
			break;
		}
		if (!sk_dev_equal_l3scope(sk, pkt.ipi6_ifindex))
			goto e_inval;

		np->sticky_pktinfo.ipi6_ifindex = pkt.ipi6_ifindex;
		np->sticky_pktinfo.ipi6_addr = pkt.ipi6_addr;
		retv = 0;
		break;
	}

	case IPV6_2292PKTOPTIONS:
	{
		struct ipv6_txoptions *opt = NULL;
		struct msghdr msg;
		struct flowi6 fl6;
		struct ipcm6_cookie ipc6;

		memset(&fl6, 0, sizeof(fl6));
		fl6.flowi6_oif = sk->sk_bound_dev_if;
		fl6.flowi6_mark = sk->sk_mark;

		if (optlen == 0)
			goto update;

		/* 1K is probably excessive
		 * 1K is surely not enough, 2K per standard header is 16K.
		 */
		retv = -EINVAL;
		if (optlen > 64*1024)
			break;

		opt = sock_kmalloc(sk, sizeof(*opt) + optlen, GFP_KERNEL);
		retv = -ENOBUFS;
		if (!opt)
			break;

		memset(opt, 0, sizeof(*opt));
		refcount_set(&opt->refcnt, 1);
		opt->tot_len = sizeof(*opt) + optlen;
		retv = -EFAULT;
		if (copy_from_sockptr(opt + 1, optval, optlen))
			goto done;

		msg.msg_controllen = optlen;
		msg.msg_control_is_user = false;
		msg.msg_control = (void *)(opt+1);
		ipc6.opt = opt;

		retv = ip6_datagram_send_ctl(net, sk, &msg, &fl6, &ipc6);
		if (retv)
			goto done;
update:
		retv = 0;
		opt = ipv6_update_options(sk, opt);
done:
		if (opt) {
			atomic_sub(opt->tot_len, &sk->sk_omem_alloc);
			txopt_put(opt);
		}
		break;
	}

	case IPV6_ADD_MEMBERSHIP:
	case IPV6_DROP_MEMBERSHIP:
	{
		struct ipv6_mreq mreq;

		if (optlen < sizeof(struct ipv6_mreq))
			goto e_inval;

		retv = -EPROTO;
		if (inet_test_bit(IS_ICSK, sk))
			break;

		retv = -EFAULT;
		if (copy_from_sockptr(&mreq, optval, sizeof(struct ipv6_mreq)))
			break;

		if (optname == IPV6_ADD_MEMBERSHIP)
			retv = ipv6_sock_mc_join(sk, mreq.ipv6mr_ifindex, &mreq.ipv6mr_multiaddr);
		else
			retv = ipv6_sock_mc_drop(sk, mreq.ipv6mr_ifindex, &mreq.ipv6mr_multiaddr);
		break;
	}
	case IPV6_JOIN_ANYCAST:
	case IPV6_LEAVE_ANYCAST:
	{
		struct ipv6_mreq mreq;

		if (optlen < sizeof(struct ipv6_mreq))
			goto e_inval;

		retv = -EFAULT;
		if (copy_from_sockptr(&mreq, optval, sizeof(struct ipv6_mreq)))
			break;

		if (optname == IPV6_JOIN_ANYCAST)
			retv = ipv6_sock_ac_join(sk, mreq.ipv6mr_ifindex, &mreq.ipv6mr_acaddr);
		else
			retv = ipv6_sock_ac_drop(sk, mreq.ipv6mr_ifindex, &mreq.ipv6mr_acaddr);
		break;
	}
	case MCAST_JOIN_GROUP:
	case MCAST_LEAVE_GROUP:
		if (in_compat_syscall())
			retv = compat_ipv6_mcast_join_leave(sk, optname, optval,
							    optlen);
		else
			retv = ipv6_mcast_join_leave(sk, optname, optval,
						     optlen);
		break;
	case MCAST_JOIN_SOURCE_GROUP:
	case MCAST_LEAVE_SOURCE_GROUP:
	case MCAST_BLOCK_SOURCE:
	case MCAST_UNBLOCK_SOURCE:
		retv = do_ipv6_mcast_group_source(sk, optname, optval, optlen);
		break;
	case MCAST_MSFILTER:
		if (in_compat_syscall())
			retv = compat_ipv6_set_mcast_msfilter(sk, optval,
							      optlen);
		else
			retv = ipv6_set_mcast_msfilter(sk, optval, optlen);
		break;
	case IPV6_ROUTER_ALERT:
		if (optlen < sizeof(int))
			goto e_inval;
		retv = ip6_ra_control(sk, val);
		if (retv == 0)
			inet6_assign_bit(RTALERT, sk, valbool);
		break;
	case IPV6_FLOWLABEL_MGR:
		retv = ipv6_flowlabel_opt(sk, optval, optlen);
		break;
	case IPV6_IPSEC_POLICY:
	case IPV6_XFRM_POLICY:
		retv = -EPERM;
		if (!sockopt_ns_capable(net->user_ns, CAP_NET_ADMIN))
			break;
		retv = xfrm_user_policy(sk, optname, optval, optlen);
		break;

	case IPV6_RECVFRAGSIZE:
		np->rxopt.bits.recvfragsize = valbool;
		retv = 0;
		break;
	}

unlock:
	sockopt_release_sock(sk);

	return retv;

e_inval:
	retv = -EINVAL;
	goto unlock;
}

int ipv6_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval,
		    unsigned int optlen)
{
	int err;

	if (level == SOL_IP && sk->sk_type != SOCK_RAW)
		return ip_setsockopt(sk, level, optname, optval, optlen);

	if (level != SOL_IPV6)
		return -ENOPROTOOPT;

	err = do_ipv6_setsockopt(sk, level, optname, optval, optlen);
#ifdef CONFIG_NETFILTER
	/* we need to exclude all possible ENOPROTOOPTs except default case */
	if (err == -ENOPROTOOPT && optname != IPV6_IPSEC_POLICY &&
			optname != IPV6_XFRM_POLICY)
		err = nf_setsockopt(sk, PF_INET6, optname, optval, optlen);
#endif
	return err;
}
EXPORT_SYMBOL(ipv6_setsockopt);

static int ipv6_getsockopt_sticky(struct sock *sk, struct ipv6_txoptions *opt,
				  int optname, sockptr_t optval, int len)
{
	struct ipv6_opt_hdr *hdr;

	if (!opt)
		return 0;

	switch (optname) {
	case IPV6_HOPOPTS:
		hdr = opt->hopopt;
		break;
	case IPV6_RTHDRDSTOPTS:
		hdr = opt->dst0opt;
		break;
	case IPV6_RTHDR:
		hdr = (struct ipv6_opt_hdr *)opt->srcrt;
		break;
	case IPV6_DSTOPTS:
		hdr = opt->dst1opt;
		break;
	default:
		return -EINVAL;	/* should not happen */
	}

	if (!hdr)
		return 0;

	len = min_t(unsigned int, len, ipv6_optlen(hdr));
	if (copy_to_sockptr(optval, hdr, len))
		return -EFAULT;
	return len;
}

static int ipv6_get_msfilter(struct sock *sk, sockptr_t optval,
			     sockptr_t optlen, int len)
{
	const int size0 = offsetof(struct group_filter, gf_slist_flex);
	struct group_filter gsf;
	int num;
	int err;

	if (len < size0)
		return -EINVAL;
	if (copy_from_sockptr(&gsf, optval, size0))
		return -EFAULT;
	if (gsf.gf_group.ss_family != AF_INET6)
		return -EADDRNOTAVAIL;
	num = gsf.gf_numsrc;
	sockopt_lock_sock(sk);
	err = ip6_mc_msfget(sk, &gsf, optval, size0);
	if (!err) {
		if (num > gsf.gf_numsrc)
			num = gsf.gf_numsrc;
		len = GROUP_FILTER_SIZE(num);
		if (copy_to_sockptr(optlen, &len, sizeof(int)) ||
		    copy_to_sockptr(optval, &gsf, size0))
			err = -EFAULT;
	}
	sockopt_release_sock(sk);
	return err;
}

static int compat_ipv6_get_msfilter(struct sock *sk, sockptr_t optval,
				    sockptr_t optlen, int len)
{
	const int size0 = offsetof(struct compat_group_filter, gf_slist_flex);
	struct compat_group_filter gf32;
	struct group_filter gf;
	int err;
	int num;

	if (len < size0)
		return -EINVAL;

	if (copy_from_sockptr(&gf32, optval, size0))
		return -EFAULT;
	gf.gf_interface = gf32.gf_interface;
	gf.gf_fmode = gf32.gf_fmode;
	num = gf.gf_numsrc = gf32.gf_numsrc;
	gf.gf_group = gf32.gf_group;

	if (gf.gf_group.ss_family != AF_INET6)
		return -EADDRNOTAVAIL;

	sockopt_lock_sock(sk);
	err = ip6_mc_msfget(sk, &gf, optval, size0);
	sockopt_release_sock(sk);
	if (err)
		return err;
	if (num > gf.gf_numsrc)
		num = gf.gf_numsrc;
	len = GROUP_FILTER_SIZE(num) - (sizeof(gf)-sizeof(gf32));
	if (copy_to_sockptr(optlen, &len, sizeof(int)) ||
	    copy_to_sockptr_offset(optval, offsetof(struct compat_group_filter, gf_fmode),
				   &gf.gf_fmode, sizeof(gf32.gf_fmode)) ||
	    copy_to_sockptr_offset(optval, offsetof(struct compat_group_filter, gf_numsrc),
				   &gf.gf_numsrc, sizeof(gf32.gf_numsrc)))
		return -EFAULT;
	return 0;
}

int do_ipv6_getsockopt(struct sock *sk, int level, int optname,
		       sockptr_t optval, sockptr_t optlen)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	int len;
	int val;

	if (ip6_mroute_opt(optname))
		return ip6_mroute_getsockopt(sk, optname, optval, optlen);

	if (copy_from_sockptr(&len, optlen, sizeof(int)))
		return -EFAULT;
	switch (optname) {
	case IPV6_ADDRFORM:
		if (sk->sk_protocol != IPPROTO_UDP &&
		    sk->sk_protocol != IPPROTO_UDPLITE &&
		    sk->sk_protocol != IPPROTO_TCP)
			return -ENOPROTOOPT;
		if (sk->sk_state != TCP_ESTABLISHED)
			return -ENOTCONN;
		val = sk->sk_family;
		break;
	case MCAST_MSFILTER:
		if (in_compat_syscall())
			return compat_ipv6_get_msfilter(sk, optval, optlen, len);
		return ipv6_get_msfilter(sk, optval, optlen, len);
	case IPV6_2292PKTOPTIONS:
	{
		struct msghdr msg;
		struct sk_buff *skb;

		if (sk->sk_type != SOCK_STREAM)
			return -ENOPROTOOPT;

		if (optval.is_kernel) {
			msg.msg_control_is_user = false;
			msg.msg_control = optval.kernel;
		} else {
			msg.msg_control_is_user = true;
			msg.msg_control_user = optval.user;
		}
		msg.msg_controllen = len;
		msg.msg_flags = 0;

		sockopt_lock_sock(sk);
		skb = np->pktoptions;
		if (skb)
			ip6_datagram_recv_ctl(sk, &msg, skb);
		sockopt_release_sock(sk);
		if (!skb) {
			if (np->rxopt.bits.rxinfo) {
				int mcast_oif = READ_ONCE(np->mcast_oif);
				struct in6_pktinfo src_info;

				src_info.ipi6_ifindex = mcast_oif ? :
					np->sticky_pktinfo.ipi6_ifindex;
				src_info.ipi6_addr = mcast_oif ? sk->sk_v6_daddr : np->sticky_pktinfo.ipi6_addr;
				put_cmsg(&msg, SOL_IPV6, IPV6_PKTINFO, sizeof(src_info), &src_info);
			}
			if (np->rxopt.bits.rxhlim) {
				int hlim = READ_ONCE(np->mcast_hops);

				put_cmsg(&msg, SOL_IPV6, IPV6_HOPLIMIT, sizeof(hlim), &hlim);
			}
			if (np->rxopt.bits.rxtclass) {
				int tclass = (int)ip6_tclass(np->rcv_flowinfo);

				put_cmsg(&msg, SOL_IPV6, IPV6_TCLASS, sizeof(tclass), &tclass);
			}
			if (np->rxopt.bits.rxoinfo) {
				int mcast_oif = READ_ONCE(np->mcast_oif);
				struct in6_pktinfo src_info;

				src_info.ipi6_ifindex = mcast_oif ? :
					np->sticky_pktinfo.ipi6_ifindex;
				src_info.ipi6_addr = mcast_oif ? sk->sk_v6_daddr :
								 np->sticky_pktinfo.ipi6_addr;
				put_cmsg(&msg, SOL_IPV6, IPV6_2292PKTINFO, sizeof(src_info), &src_info);
			}
			if (np->rxopt.bits.rxohlim) {
				int hlim = READ_ONCE(np->mcast_hops);

				put_cmsg(&msg, SOL_IPV6, IPV6_2292HOPLIMIT, sizeof(hlim), &hlim);
			}
			if (np->rxopt.bits.rxflow) {
				__be32 flowinfo = np->rcv_flowinfo;

				put_cmsg(&msg, SOL_IPV6, IPV6_FLOWINFO, sizeof(flowinfo), &flowinfo);
			}
		}
		len -= msg.msg_controllen;
		return copy_to_sockptr(optlen, &len, sizeof(int));
	}
	case IPV6_MTU:
	{
		struct dst_entry *dst;

		val = 0;
		rcu_read_lock();
		dst = __sk_dst_get(sk);
		if (dst)
			val = dst_mtu(dst);
		rcu_read_unlock();
		if (!val)
			return -ENOTCONN;
		break;
	}

	case IPV6_V6ONLY:
		val = sk->sk_ipv6only;
		break;

	case IPV6_RECVPKTINFO:
		val = np->rxopt.bits.rxinfo;
		break;

	case IPV6_2292PKTINFO:
		val = np->rxopt.bits.rxoinfo;
		break;

	case IPV6_RECVHOPLIMIT:
		val = np->rxopt.bits.rxhlim;
		break;

	case IPV6_2292HOPLIMIT:
		val = np->rxopt.bits.rxohlim;
		break;

	case IPV6_RECVRTHDR:
		val = np->rxopt.bits.srcrt;
		break;

	case IPV6_2292RTHDR:
		val = np->rxopt.bits.osrcrt;
		break;

	case IPV6_HOPOPTS:
	case IPV6_RTHDRDSTOPTS:
	case IPV6_RTHDR:
	case IPV6_DSTOPTS:
	{
		struct ipv6_txoptions *opt;

		sockopt_lock_sock(sk);
		opt = rcu_dereference_protected(np->opt,
						lockdep_sock_is_held(sk));
		len = ipv6_getsockopt_sticky(sk, opt, optname, optval, len);
		sockopt_release_sock(sk);
		/* check if ipv6_getsockopt_sticky() returns err code */
		if (len < 0)
			return len;
		return copy_to_sockptr(optlen, &len, sizeof(int));
	}

	case IPV6_RECVHOPOPTS:
		val = np->rxopt.bits.hopopts;
		break;

	case IPV6_2292HOPOPTS:
		val = np->rxopt.bits.ohopopts;
		break;

	case IPV6_RECVDSTOPTS:
		val = np->rxopt.bits.dstopts;
		break;

	case IPV6_2292DSTOPTS:
		val = np->rxopt.bits.odstopts;
		break;

	case IPV6_TCLASS:
		val = np->tclass;
		break;

	case IPV6_RECVTCLASS:
		val = np->rxopt.bits.rxtclass;
		break;

	case IPV6_FLOWINFO:
		val = np->rxopt.bits.rxflow;
		break;

	case IPV6_RECVPATHMTU:
		val = np->rxopt.bits.rxpmtu;
		break;

	case IPV6_PATHMTU:
	{
		struct dst_entry *dst;
		struct ip6_mtuinfo mtuinfo;

		if (len < sizeof(mtuinfo))
			return -EINVAL;

		len = sizeof(mtuinfo);
		memset(&mtuinfo, 0, sizeof(mtuinfo));

		rcu_read_lock();
		dst = __sk_dst_get(sk);
		if (dst)
			mtuinfo.ip6m_mtu = dst_mtu(dst);
		rcu_read_unlock();
		if (!mtuinfo.ip6m_mtu)
			return -ENOTCONN;

		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &mtuinfo, len))
			return -EFAULT;

		return 0;
	}

	case IPV6_TRANSPARENT:
		val = inet_test_bit(TRANSPARENT, sk);
		break;

	case IPV6_FREEBIND:
		val = inet_test_bit(FREEBIND, sk);
		break;

	case IPV6_RECVORIGDSTADDR:
		val = np->rxopt.bits.rxorigdstaddr;
		break;

	case IPV6_UNICAST_HOPS:
	case IPV6_MULTICAST_HOPS:
	{
		struct dst_entry *dst;

		if (optname == IPV6_UNICAST_HOPS)
			val = READ_ONCE(np->hop_limit);
		else
			val = READ_ONCE(np->mcast_hops);

		if (val < 0) {
			rcu_read_lock();
			dst = __sk_dst_get(sk);
			if (dst)
				val = ip6_dst_hoplimit(dst);
			rcu_read_unlock();
		}

		if (val < 0)
			val = READ_ONCE(sock_net(sk)->ipv6.devconf_all->hop_limit);
		break;
	}

	case IPV6_MULTICAST_LOOP:
		val = inet6_test_bit(MC6_LOOP, sk);
		break;

	case IPV6_MULTICAST_IF:
		val = READ_ONCE(np->mcast_oif);
		break;

	case IPV6_MULTICAST_ALL:
		val = inet6_test_bit(MC6_ALL, sk);
		break;

	case IPV6_UNICAST_IF:
		val = (__force int)htonl((__u32) READ_ONCE(np->ucast_oif));
		break;

	case IPV6_MTU_DISCOVER:
		val = READ_ONCE(np->pmtudisc);
		break;

	case IPV6_RECVERR:
		val = inet6_test_bit(RECVERR6, sk);
		break;

	case IPV6_FLOWINFO_SEND:
		val = inet6_test_bit(SNDFLOW, sk);
		break;

	case IPV6_FLOWLABEL_MGR:
	{
		struct in6_flowlabel_req freq;
		int flags;

		if (len < sizeof(freq))
			return -EINVAL;

		if (copy_from_sockptr(&freq, optval, sizeof(freq)))
			return -EFAULT;

		if (freq.flr_action != IPV6_FL_A_GET)
			return -EINVAL;

		len = sizeof(freq);
		flags = freq.flr_flags;

		memset(&freq, 0, sizeof(freq));

		val = ipv6_flowlabel_opt_get(sk, &freq, flags);
		if (val < 0)
			return val;

		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &freq, len))
			return -EFAULT;

		return 0;
	}

	case IPV6_ADDR_PREFERENCES:
		{
		u8 srcprefs = READ_ONCE(np->srcprefs);
		val = 0;

		if (srcprefs & IPV6_PREFER_SRC_TMP)
			val |= IPV6_PREFER_SRC_TMP;
		else if (srcprefs & IPV6_PREFER_SRC_PUBLIC)
			val |= IPV6_PREFER_SRC_PUBLIC;
		else {
			/* XXX: should we return system default? */
			val |= IPV6_PREFER_SRC_PUBTMP_DEFAULT;
		}

		if (srcprefs & IPV6_PREFER_SRC_COA)
			val |= IPV6_PREFER_SRC_COA;
		else
			val |= IPV6_PREFER_SRC_HOME;
		break;
		}
	case IPV6_MINHOPCOUNT:
		val = READ_ONCE(np->min_hopcount);
		break;

	case IPV6_DONTFRAG:
		val = inet6_test_bit(DONTFRAG, sk);
		break;

	case IPV6_AUTOFLOWLABEL:
		val = ip6_autoflowlabel(sock_net(sk), sk);
		break;

	case IPV6_RECVFRAGSIZE:
		val = np->rxopt.bits.recvfragsize;
		break;

	case IPV6_ROUTER_ALERT:
		val = inet6_test_bit(RTALERT, sk);
		break;

	case IPV6_ROUTER_ALERT_ISOLATE:
		val = inet6_test_bit(RTALERT_ISOLATE, sk);
		break;

	case IPV6_RECVERR_RFC4884:
		val = inet6_test_bit(RECVERR6_RFC4884, sk);
		break;

	default:
		return -ENOPROTOOPT;
	}
	len = min_t(unsigned int, sizeof(int), len);
	if (copy_to_sockptr(optlen, &len, sizeof(int)))
		return -EFAULT;
	if (copy_to_sockptr(optval, &val, len))
		return -EFAULT;
	return 0;
}

int ipv6_getsockopt(struct sock *sk, int level, int optname,
		    char __user *optval, int __user *optlen)
{
	int err;

	if (level == SOL_IP && sk->sk_type != SOCK_RAW)
		return ip_getsockopt(sk, level, optname, optval, optlen);

	if (level != SOL_IPV6)
		return -ENOPROTOOPT;

	err = do_ipv6_getsockopt(sk, level, optname,
				 USER_SOCKPTR(optval), USER_SOCKPTR(optlen));
#ifdef CONFIG_NETFILTER
	/* we need to exclude all possible ENOPROTOOPTs except default case */
	if (err == -ENOPROTOOPT && optname != IPV6_2292PKTOPTIONS) {
		int len;

		if (get_user(len, optlen))
			return -EFAULT;

		err = nf_getsockopt(sk, PF_INET6, optname, optval, &len);
		if (err >= 0)
			err = put_user(len, optlen);
	}
#endif
	return err;
}
EXPORT_SYMBOL(ipv6_getsockopt);
