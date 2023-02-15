/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Command to manage vnifiltering on a vxlan device
 *
 * Authors:     Roopa Prabhu <roopa@nvidia.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_link.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>

#include "json_print.h"
#include "libnetlink.h"
#include "br_common.h"
#include "utils.h"

static unsigned int filter_index;

#define VXLAN_ID_LEN 15

#define __stringify_1(x...) #x
#define __stringify(x...) __stringify_1(x)

static void usage(void)
{
	fprintf(stderr,
		"Usage: bridge vni { add | del } vni VNI\n"
		"		[ { group | remote } IP_ADDRESS ]\n"
	        "		[ dev DEV ]\n"
		"       bridge vni { show }\n"
		"\n"
		"Where:	VNI	:= 0-16777215\n"
	       );
	exit(-1);
}

static int parse_vni_filter(const char *argv, struct nlmsghdr *n, int reqsize,
			    inet_prefix *group)
{
	char *vnilist = strdupa(argv);
	char *vni = strtok(vnilist, ",");
	int group_type = AF_UNSPEC;
	struct rtattr *nlvlist_e;
	char *v;
	int i;

	if (group && is_addrtype_inet(group))
		group_type = (group->family == AF_INET) ?  VXLAN_VNIFILTER_ENTRY_GROUP :
						     VXLAN_VNIFILTER_ENTRY_GROUP6;

	for (i = 0; vni; i++) {
		__u32 vni_start = 0, vni_end = 0;

		v = strchr(vni, '-');
		if (v) {
			*v = '\0';
			v++;
			vni_start = atoi(vni);
			vni_end = atoi(v);
		} else {
			vni_start = atoi(vni);
		}
		nlvlist_e = addattr_nest(n, reqsize, VXLAN_VNIFILTER_ENTRY |
					 NLA_F_NESTED);
		addattr32(n, 1024, VXLAN_VNIFILTER_ENTRY_START, vni_start);
		if (vni_end)
			addattr32(n, 1024, VXLAN_VNIFILTER_ENTRY_END, vni_end);
		if (group)
			addattr_l(n, 1024, group_type, group->data, group->bytelen);
		addattr_nest_end(n, nlvlist_e);
		vni = strtok(NULL, ",");
	}

	return 0;
}

static int vni_modify(int cmd, int argc, char **argv)
{
	struct {
		struct nlmsghdr	n;
		struct tunnel_msg	tmsg;
		char			buf[1024];
	} req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tunnel_msg)),
		.n.nlmsg_flags = NLM_F_REQUEST,
		.n.nlmsg_type = cmd,
		.tmsg.family = PF_BRIDGE,
	};
	bool group_present = false;
	inet_prefix daddr;
	char *vni = NULL;
	char *d = NULL;

	while (argc > 0) {
		if (strcmp(*argv, "dev") == 0) {
			NEXT_ARG();
			d = *argv;
		} else if (strcmp(*argv, "vni") == 0) {
			NEXT_ARG();
			if (vni)
				invarg("duplicate vni", *argv);
			vni = *argv;
		} else if (strcmp(*argv, "group") == 0) {
			if (group_present)
				invarg("duplicate group", *argv);
			if (is_addrtype_inet_not_multi(&daddr)) {
				fprintf(stderr, "vxlan: both group and remote");
				fprintf(stderr, " cannot be specified\n");
				return -1;
			}
			NEXT_ARG();
			get_addr(&daddr, *argv, AF_UNSPEC);
			if (!is_addrtype_inet_multi(&daddr))
				invarg("invalid group address", *argv);
			group_present = true;
		} else if (strcmp(*argv, "remote") == 0) {
			if (group_present)
				invarg("duplicate group", *argv);
			NEXT_ARG();
			get_addr(&daddr, *argv, AF_UNSPEC);
			group_present = true;
		} else {
			if (strcmp(*argv, "help") == 0)
				usage();
		}
		argc--; argv++;
	}

	if (d == NULL || vni == NULL) {
		fprintf(stderr, "Device and VNI ID are required arguments.\n");
		return -1;
	}

	if (!vni && group_present) {
		fprintf(stderr, "Group can only be specified with a vni\n");
		return -1;
	}

	if (vni)
		parse_vni_filter(vni, &req.n, sizeof(req),
				 (group_present ? &daddr : NULL));

	req.tmsg.ifindex = ll_name_to_index(d);
	if (req.tmsg.ifindex == 0) {
		fprintf(stderr, "Cannot find vxlan device \"%s\"\n", d);
		return -1;
	}

	if (rtnl_talk(&rth, &req.n, NULL) < 0)
		return -1;

	return 0;
}

static void open_vni_port(int ifi_index, const char *fmt)
{
	open_json_object(NULL);
	print_color_string(PRINT_ANY, COLOR_IFNAME, "ifname",
			   "%-" __stringify(IFNAMSIZ) "s  ",
			   ll_index_to_name(ifi_index));
	open_json_array(PRINT_JSON, "vnis");
}

static void close_vni_port(void)
{
	close_json_array(PRINT_JSON, NULL);
	close_json_object();
}

static void print_range(const char *name, __u32 start, __u32 id)
{
	char end[64];

	snprintf(end, sizeof(end), "%sEnd", name);

	print_uint(PRINT_ANY, name, " %u", start);
	if (start != id)
		print_uint(PRINT_ANY, end, "-%-14u ", id);

}

static void print_vnifilter_entry_stats(struct rtattr *stats_attr)
{
	struct rtattr *stb[VNIFILTER_ENTRY_STATS_MAX+1];
	__u64 stat;

	open_json_object("stats");
	parse_rtattr_flags(stb, VNIFILTER_ENTRY_STATS_MAX, RTA_DATA(stats_attr),
			   RTA_PAYLOAD(stats_attr), NLA_F_NESTED);

	print_nl();
	print_string(PRINT_FP, NULL, "%-" __stringify(IFNAMSIZ) "s   ", "");
	print_string(PRINT_FP, NULL, "RX: ", "");

	if (stb[VNIFILTER_ENTRY_STATS_RX_BYTES]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_RX_BYTES]);
		print_lluint(PRINT_ANY, "rx_bytes", "bytes %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_RX_PKTS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_RX_PKTS]);
		print_lluint(PRINT_ANY, "rx_pkts", "pkts %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_RX_DROPS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_RX_DROPS]);
		print_lluint(PRINT_ANY, "rx_drops", "drops %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_RX_ERRORS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_RX_ERRORS]);
		print_lluint(PRINT_ANY, "rx_errors", "errors %llu ", stat);
	}

	print_nl();
	print_string(PRINT_FP, NULL, "%-" __stringify(IFNAMSIZ) "s   ", "");
	print_string(PRINT_FP, NULL, "TX: ", "");

	if (stb[VNIFILTER_ENTRY_STATS_TX_BYTES]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_TX_BYTES]);
		print_lluint(PRINT_ANY, "tx_bytes", "bytes %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_TX_PKTS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_TX_PKTS]);
		print_lluint(PRINT_ANY, "tx_pkts", "pkts %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_TX_DROPS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_TX_DROPS]);
		print_lluint(PRINT_ANY, "tx_drops", "drops %llu ", stat);
	}
	if (stb[VNIFILTER_ENTRY_STATS_TX_ERRORS]) {
		stat = rta_getattr_u64(stb[VNIFILTER_ENTRY_STATS_TX_ERRORS]);
		print_lluint(PRINT_ANY, "tx_errors", "errors %llu ", stat);
	}
	close_json_object();
}

static void print_vni(struct rtattr *t, int ifindex)
{
	struct rtattr *ttb[VXLAN_VNIFILTER_ENTRY_MAX+1];
	__u32 vni_start = 0;
	__u32 vni_end = 0;

	parse_rtattr_flags(ttb, VXLAN_VNIFILTER_ENTRY_MAX, RTA_DATA(t),
			   RTA_PAYLOAD(t), NLA_F_NESTED);

	if (ttb[VXLAN_VNIFILTER_ENTRY_START])
		vni_start = rta_getattr_u32(ttb[VXLAN_VNIFILTER_ENTRY_START]);

	if (ttb[VXLAN_VNIFILTER_ENTRY_END])
		vni_end = rta_getattr_u32(ttb[VXLAN_VNIFILTER_ENTRY_END]);

	if (vni_end)
		print_range("vni", vni_start, vni_end);
	else
		print_uint(PRINT_ANY, "vni", " %-14u", vni_start);

	if (ttb[VXLAN_VNIFILTER_ENTRY_GROUP]) {
		__be32 addr = rta_getattr_u32(ttb[VXLAN_VNIFILTER_ENTRY_GROUP]);

		if (addr) {
			if (IN_MULTICAST(ntohl(addr)))
				print_string(PRINT_ANY,
					     "group",
					     " %s",
					     format_host(AF_INET, 4, &addr));
			else
				print_string(PRINT_ANY,
					     "remote",
					     " %s",
					     format_host(AF_INET, 4, &addr));
		}
	} else if (ttb[VXLAN_VNIFILTER_ENTRY_GROUP6]) {
		struct in6_addr addr;

		memcpy(&addr, RTA_DATA(ttb[VXLAN_VNIFILTER_ENTRY_GROUP6]), sizeof(struct in6_addr));
		if (!IN6_IS_ADDR_UNSPECIFIED(&addr)) {
			if (IN6_IS_ADDR_MULTICAST(&addr))
				print_string(PRINT_ANY,
					     "group",
					     " %s",
					     format_host(AF_INET6,
							 sizeof(struct in6_addr),
							 &addr));
			else
				print_string(PRINT_ANY,
					     "remote",
					     " %s",
					     format_host(AF_INET6,
							 sizeof(struct in6_addr),
							 &addr));
		}
	}

	if (ttb[VXLAN_VNIFILTER_ENTRY_STATS])
		print_vnifilter_entry_stats(ttb[VXLAN_VNIFILTER_ENTRY_STATS]);

	close_json_object();
	print_string(PRINT_FP, NULL, "%s", _SL_);
}

int print_vnifilter_rtm(struct nlmsghdr *n, void *arg)
{
	struct tunnel_msg *tmsg = NLMSG_DATA(n);
	int len = n->nlmsg_len;
	bool first = true;
	struct rtattr *t;
	FILE *fp = arg;
	int rem;

	if (n->nlmsg_type != RTM_NEWTUNNEL &&
	    n->nlmsg_type != RTM_DELTUNNEL &&
	    n->nlmsg_type != RTM_GETTUNNEL) {
		fprintf(stderr, "Unknown vni tunnel rtm msg: %08x %08x %08x\n",
			n->nlmsg_len, n->nlmsg_type, n->nlmsg_flags);
		return 0;
	}

	len -= NLMSG_LENGTH(sizeof(*tmsg));
	if (len < 0) {
		fprintf(stderr, "BUG: wrong nlmsg len %d\n", len);
		return -1;
	}

	if (tmsg->family != AF_BRIDGE)
		return 0;

	if (filter_index && filter_index != tmsg->ifindex)
		return 0;

	print_headers(fp, "[TUNNEL]");

	if (n->nlmsg_type == RTM_DELTUNNEL)
		print_bool(PRINT_ANY, "deleted", "Deleted ", true);

	rem = len;
	for (t = TUNNEL_RTA(tmsg); RTA_OK(t, rem); t = RTA_NEXT(t, rem)) {
		unsigned short rta_type = t->rta_type & NLA_TYPE_MASK;

		if (rta_type != VXLAN_VNIFILTER_ENTRY)
			continue;
		if (first) {
			open_vni_port(tmsg->ifindex, "%s");
			open_json_object(NULL);
			first = false;
		} else {
			open_json_object(NULL);
			print_string(PRINT_FP, NULL, "%-" __stringify(IFNAMSIZ) "s  ", "");
		}

		print_vni(t, tmsg->ifindex);
	}
	close_vni_port();

	print_string(PRINT_FP, NULL, "%s", _SL_);

	fflush(stdout);
	return 0;
}

static int print_vnifilter_rtm_filter(struct nlmsghdr *n, void *arg)
{
	return print_vnifilter_rtm(n, arg);
}

static int vni_show(int argc, char **argv)
{
	char *filter_dev = NULL;
	__u8 flags = 0;
	int ret = 0;

	while (argc > 0) {
		if (strcmp(*argv, "dev") == 0) {
			NEXT_ARG();
			if (filter_dev)
				duparg("dev", *argv);
			filter_dev = *argv;
		}
		argc--; argv++;
	}

	if (filter_dev) {
		filter_index = ll_name_to_index(filter_dev);
		if (!filter_index)
			return nodev(filter_dev);
	}

	new_json_obj(json);

	if (show_stats)
		flags = TUNNEL_MSG_FLAG_STATS;

	if (rtnl_tunneldump_req(&rth, PF_BRIDGE, filter_index, flags) < 0) {
		perror("Cannot send dump request");
		exit(1);
	}

	if (!is_json_context()) {
		printf("%-" __stringify(IFNAMSIZ) "s  %-"
		       __stringify(VXLAN_ID_LEN) "s  %-"
		       __stringify(15) "s",
		       "dev", "vni", "group/remote");
		printf("\n");
	}

	ret = rtnl_dump_filter(&rth, print_vnifilter_rtm_filter, NULL);
	if (ret < 0) {
		fprintf(stderr, "Dump ternminated\n");
		exit(1);
	}

	delete_json_obj();
	fflush(stdout);
	return 0;
}

int do_vni(int argc, char **argv)
{
	ll_init_map(&rth);
	timestamp = 0;

	if (argc > 0) {
		if (strcmp(*argv, "add") == 0)
			return vni_modify(RTM_NEWTUNNEL, argc-1, argv+1);
		if (strcmp(*argv, "delete") == 0)
			return vni_modify(RTM_DELTUNNEL, argc-1, argv+1);
		if (strcmp(*argv, "show") == 0 ||
		    strcmp(*argv, "lst") == 0 ||
		    strcmp(*argv, "list") == 0)
			return vni_show(argc-1, argv+1);
		if (strcmp(*argv, "help") == 0)
			usage();
	} else {
		return vni_show(0, NULL);
	}

	fprintf(stderr, "Command \"%s\" is unknown, try \"bridge vni help\".\n", *argv);
	exit(-1);
}
