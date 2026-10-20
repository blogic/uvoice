// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per line SIP account (src/proto/sip/account.c).
 *
 * What it binds. One channel of /etc/config/voip to one registration client:
 * the derived identity (address of record, registrar request URI, the next hop
 * to resolve), a transaction manager on a UDP transport socket and the async
 * resolver. It asks the core firewall reconcile for its signaling rules
 * (net/fw_reconcile.h) and follows the voice interface through the core
 * netifd tracker (net/iface.h). The SIP protocol builds it on startup and
 * rebuilds it on reload, and it is the graceful stop owner for the
 * registration side (Expires 0 on SIGTERM).
 *
 * Threading. Everything here runs on the uloop thread. The transport, the
 * resolver and the transaction layer are all uloop native.
 *
 * Testability. The pure parts, the fallback chain that derives an account from
 * a channel, the apply class of a config change, and the map from a reg state
 * to a status word, are exported so a unit test drives them without a socket,
 * a uloop or ubus. The socket, resolver and netifd paths are exercised by the
 * host live registration harness, not by the unit suite.
 */
#ifndef UVOICE_ACCOUNT_H
#define UVOICE_ACCOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "proto/proto.h"
/* for struct sip_txn_dst by value in struct acct_linger_entry */
#include "proto/sip/txn.h"

struct blob_attr;
struct blob_buf;
struct sip_conn;
struct sip_sub;
struct sip_dns;
struct sip_dns_result;
struct sip_reg;
struct sip_route_hdrs;
struct sip_transport;
struct ubus_context;

#define ACCT_AOR_MAX		256
#define ACCT_URI_MAX		160
#define ACCT_HOST_MAX		128
#define ACCT_USER_MAX		96
#define ACCT_PASS_MAX		128
#define ACCT_IFACE_MAX		16

/*
 * The apply class of a config change, mirroring cfg/schema.h so the reload
 * does the least work that is still correct. Higher ordinals subsume lower
 * ones. ACCT_APPLY_RESTART covers both the schema RESTART_LINE (a new local
 * port or TOS) and RESTART_ALL at the level of one line (a new interface or
 * transport): both mean the transport socket is rebuilt, so the account layer
 * treats them the same. A base level RESTART_ALL (enabled, channels,
 * local_interface) is handled by rebuilding every account in accounts_reload().
 */
enum acct_apply {
	ACCT_APPLY_NONE = 0,
	ACCT_APPLY_LIVE,		/* only timers moved: update in place */
	ACCT_APPLY_REREG,		/* identity or auth moved: rebuild reg */
	ACCT_APPLY_RESTART,		/* transport moved: rebuild socket and reg */
};

/*
 * The transport of one line, from the uci transport option. UDP is 0, so a
 * zero initialised configuration is the transport uvoice has always used.
 *
 * Transport is base only. acct_cfg_derive() reads transport from the base
 * section alone. A channel that names a different value warns once and is
 * ignored, unlike every other alias option. The base only rule follows the
 * firewall reconcile, which writes one proto per rule set, so two lines on two
 * transports would silently break one line's inbound SIP.
 */
enum acct_transport {
	ACCT_TP_UDP = 0,
	ACCT_TP_TCP,
	ACCT_TP_TLS,
};

/*
 * The Via and Contact token of one transport: NULL for UDP, and the uppercase
 * "TCP" or "TLS" the Via carries after "SIP/2.0/" (sip/reg.h, sip/keepalive.h,
 * sip/sub.h and acct_call_view below all take exactly this; the Contact writer
 * lowercases it for ";transport="). The four writer sites of account.c take
 * their member from here, so one vocabulary serves all of them. It returns
 * static literals, which the writers may hold past the account that handed
 * them out; exported so test_account.c pins the mapping without a live line.
 */
const char *acct_transport_token(enum acct_transport tp);

/*
 * The RFC 3263 4.1 SRV service label one transport resolves with:
 * "_sip._udp", "_sip._tcp", "_sips._tcp" (shared/resolve.h). The label joins
 * the resolver's cache identity, so a wrong one here does not fail, it serves
 * another transport's targets. Exported so test_account.c pins the three rows.
 */
const char *acct_dns_service(enum acct_transport tp);

/*
 * The enum sip_stream_kind one transport opens: TLS for tls, TCP for the rest.
 * It is an int here for the reason acct_status_name() takes one: sip/stream.h
 * carries uloop and OpenSSL declarations that this header does not inherit.
 * Exported so test_account.c pins the mapping; a swap here would hand a tls
 * line a cleartext connection.
 */
int acct_stream_kind(enum acct_transport tp);

/*
 * The enum sip_dns_kind one line asks with, from the base dns_srv_mode word
 * and whether the outbound chain names a port (RFC 3263 4.2). It returns an
 * int for the reason acct_stream_kind() does. It answers SIP_DNS_SRV,
 * SIP_DNS_ADDR_SRV or SIP_DNS_ADDR and nothing else, which is what the
 * status vocabulary carries.
 * Exported so test_account.c pins the rows.
 */
int acct_dns_kind(enum cfg_dns_srv_mode mode, bool port_named);

/*
 * The immutable per line configuration the account runs on, derived from a
 * channel and the base section through the schema fallback chain. Every string
 * the registration client points at lives in here, so the account owns its
 * configuration and the loaded struct cfg can be freed after a reload.
 */
struct acct_cfg {
	bool enabled;
	unsigned int line;

	char aor[ACCT_AOR_MAX];			/* sip:user@domain */
	char registrar_uri[ACCT_URI_MAX];	/* REGISTER request URI, sip:registrar */
	char resolve_host[ACCT_HOST_MAX];	/* next hop to resolve */
	char contact_user[ACCT_USER_MAX];
	char auth_user[ACCT_USER_MAX];
	char auth_pass[ACCT_PASS_MAX];
	char display[ACCT_USER_MAX];
	char iface[ACCT_IFACE_MAX];		/* base or channel local_interface */

	uint16_t resolve_port;
	uint16_t local_port;
	uint8_t sip_tos;

	/*
	 * The transport the line runs on, and the only place it is stored. The
	 * firewall reconcile derives its proto from it (fw.c takes a bool and
	 * changes nothing for this feature), and the four Via and Contact
	 * writers take the token acct_transport_token() maps it to.
	 */
	enum acct_transport transport;

	bool dns_cache_update_mode;
	bool dns_move_unregister;

	/*
	 * The question this line asks for its next hop, an enum sip_dns_kind
	 * held as a number: this header does not inherit shared/resolve.h and
	 * an enum has no forward declaration, the reason acct_stream_kind()
	 * returns an int and struct sip_dns_query holds its own kind this way.
	 * acct_dns_kind() derives it from dns_srv_mode and the ports the line
	 * names, and both resolver call sites read it.
	 */
	uint8_t dns_kind;

	uint32_t expires;
	uint32_t retry_s;

	/*
	 * uci mwi_subscribe (base scope). It selects the SUBSCRIBE client of
	 * sip/sub.h; the unsolicited NOTIFY intake runs whatever it says. The
	 * schema calls it REREG, and the subscription is built beside the
	 * registration, so a change rebuilds both.
	 */
	bool mwi_subscribe;

	/*
	 * Keepalive (uci keepalive_enabled, keepalive_interval, and the
	 * effective options_interval after the channel alias rule). All three
	 * are the schema LIVE class: a change updates the running instance in
	 * place through sip_ka_cfg_update().
	 */
	bool ka_enabled;
	uint32_t ka_interval;
	uint32_t options_interval;

	/*
	 * uci restrict_source 2, the conntrack pinhole form. A udp line under
	 * it runs the CRLF keepalive whatever the pair above says, inside half
	 * the conntrack udp bound (acct_ka_interval_effective); the option is
	 * LIVE, so a move re-arms the running instance and rebuilds nothing.
	 */
	bool pinhole;

	/*
	 * uci dns_linger (base scope, LIVE): seconds an address a resolver
	 * answer named keeps its inbound admission after it leaves the answer.
	 * The keepalive round pings it under restrict_source 2 and mode 1
	 * keeps its rule, because a pool that rotates one record per query
	 * sends inbound requests from members the current answer does not
	 * name. 0 turns the retention off.
	 */
	unsigned int linger_s;

	unsigned int t1, t2, t4, ta, tb, td, te, tf, tg, th, ti, tj, tk;
};

/*
 * The CRLF interval of a udp line under restrict_source 2, against the
 * conntrack udp bound of net/conntrack.h (net_ct_udp_bound_s):
 * max(1, min(configured, bound_s / 2)), where configured is ka_interval when
 * the pair is on and bound_s / 2 otherwise. forced reports that the answer
 * differs from the configured pair, because it was off or slower. Exported so
 * test_account.c pins the clamp without a live line.
 */
unsigned int acct_ka_interval_effective(const struct acct_cfg *c,
					unsigned int bound_s, bool *forced);

/*
 * The dns_linger retention: every recently resolved proxy endpoint and when
 * an answer last named it. Every answer notes its endpoints, an endpoint that
 * stops appearing expires after the window, and two readers hold its inbound
 * admission until then: the keepalive round pings it under restrict_source 2,
 * and the mode 1 rule set keeps its Allow_VoIP_PROXY rule. A pool that
 * rotates one record per query sends inbound requests from members the
 * current answer does not name; without the retention exactly one member is
 * admissible at a time.
 *
 * The cap matches SIP_REG_TARGET_MAX: an answer that fills it makes the
 * retention equal to the target list, and mode 1 cannot admit more than
 * PROTO_FW_PROXY_MAX sources anyway. A full list evicts the entry an answer
 * named longest ago. The entries are ping and firewall inputs alone, never
 * registration failover targets: the target list stays the last answer of
 * record, and an address the answer no longer names is not authorized for
 * new traffic; a failed line asks DNS instead (acct_targets_exhausted()).
 */
#define ACCT_LINGER_MAX		16

struct acct_linger_entry {
	struct sip_txn_dst dst;
	uint64_t last_seen_ms;
};

struct acct_linger {
	struct acct_linger_entry entry[ACCT_LINGER_MAX];
	unsigned int count;
};

/* An upsert by identity (address and port) that refreshes last_seen_ms. */
void acct_linger_note(struct acct_linger *lg, const struct sip_txn_dst *dst,
		      uint64_t now_ms);

/* Drop every entry whose age at now_ms reached linger_s; 0 drops them all. */
void acct_linger_expire(struct acct_linger *lg, uint64_t now_ms,
			uint32_t linger_s);

bool acct_linger_covers(const struct acct_linger *lg,
			const struct sip_txn_dst *dst);

/*
 * The earliest expiry on the env clock, 0 with an empty list or a window of
 * 0. The account arms a timer with it, so a mode 1 rule outlives the window
 * by nothing even when no answer arrives and no round runs.
 */
uint64_t acct_linger_deadline(const struct acct_linger *lg, uint32_t linger_s);

/*
 * Add the lingering addresses to a source list acct_proxy_collect() began,
 * never evicting an entry already there: a rotating pool's history must not
 * push out a current proxy, so a full list drops the lingering address with
 * a warning instead. Returns the new count.
 */
unsigned int acct_linger_collect(const struct acct_linger *lg,
				 char (*list)[PROTO_FW_SRC_MAX],
				 unsigned int count);

/*
 * Derive an account from one channel and the base section. It applies the
 * schema fallback chain (the SIP port chain of cfg.h and the
 * sip_auth_username, sip_realm, sip_domain and channel alias rules) and builds
 * the address of record, the registrar request URI and the host to resolve.
 * A channel with no proxy and no domain yields enabled=false, because there is
 * nothing to register against.
 */
void acct_cfg_derive(const struct cfg *base, const struct cfg_channel *ch,
		     struct acct_cfg *out);

/*
 * The server_name a line sends, or NULL for none. Only a TLS line sends one,
 * and only when its resolve host is a name: RFC 6066 section 3 forbids a
 * literal address in the extension, and an SNI a TCP line sent would be a
 * protocol error the peer may refuse. The pointer is the account's own
 * resolve_host and lives as long as the configuration does. Exported so
 * test_account.c pins the decision without an environment.
 */
const char *acct_sni_of(const struct acct_cfg *c);

/* The apply class of the move from a to b, for the reload minimal apply. */
enum acct_apply acct_apply_class(const struct acct_cfg *a, const struct acct_cfg *b);

const char *acct_apply_name(enum acct_apply cls);

/*
 * The action a reload takes for one line, from whether it was active, whether
 * it is now wanted, and the apply class of the change. REPLACE and REMOVE both
 * unregister the old binding first; the runtime drains the Expires 0 before it
 * discards the registration, so the carrier does not keep a stale binding.
 */
enum acct_reload_op {
	ACCT_ROP_NONE = 0,
	ACCT_ROP_BUILD,		/* a new line: build it */
	ACCT_ROP_TIMERS,	/* only timers moved: update in place */
	ACCT_ROP_REPLACE,	/* graceful unregister, then rebuild on new cfg */
	ACCT_ROP_REMOVE,	/* graceful unregister, then tear down */
};

enum acct_reload_op acct_reload_action(bool was_active, bool want, enum acct_apply cls);

/*
 * True when a base option moved that rebuilds every line: enabled, channels,
 * local_interface, transport, srtp_enabled, ipv6_enabled and the five tls_*
 * options. ipv6_enabled is in the set because it decides the family of every
 * socket, Via, Contact and SDP c= line (RESTART_ALL in cfg/schema.h).
 * Exported so test_account.c pins the base-restart set without a live
 * account.
 */
bool acct_base_restart(const struct cfg *old_cfg, const struct cfg *new_cfg);

/*
 * Split "host", "host:port" or "[v6]:port" into a bare host and an embedded
 * port. The resolver takes a bare host for SRV and A, so a proxy string that
 * carries a port must never reach it whole. A port outside 1..65535, or a bare
 * IPv6 literal with no brackets, yields port 0 and the whole input as the host.
 * Returns false only for an empty input.
 */
bool acct_host_port_split(const char *in, char *host, size_t hlen, uint16_t *port);

/*
 * The coarse status word for "voice status": the six registration states fold
 * into unregistered, trying, registered and failed.
 */
const char *acct_status_name(int reg_state);

/*
 * Whether the registration state holds a live binding: REGISTERED and
 * REFRESHING, the two states that acct_status_name() reports as registered,
 * so the status word and the dial tone agree. An int for the reason that
 * acct_status_name() takes one.
 */
bool acct_reg_live(int reg_state);

struct sip_msg;
struct sip_str;

/*
 * The To user of a NOTIFY, for the account routing fallback. A switch sends
 * the message summary to the contact it was registered with, and a contact
 * URI does not have to carry the subscriber's user part at all: several
 * switches address the request URI to the device and name the subscriber in
 * To alone. It is therefore a fallback for NOTIFY only: every other method
 * keeps naming its line in the request URI, and reading To for them would let
 * an in-dialog request reach the wrong line.
 *
 * False for any other method, and for a To that carries no user.
 */
bool acct_notify_to_user(const struct sip_msg *msg, struct sip_str *user);

/*
 * Whether a request is an INVITE that carries a Replaces header, which the
 * admission of this layer reads to keep its capacity refusal and its refusal
 * for no live registration off it.
 *
 * Such an INVITE is not a new call: it frees the leg it names, so a full line
 * cannot answer it 486, and a line with no live registration does not answer
 * it 480. An empty header passes here as well; call.c counts no value in it
 * and admits it as a new call, with the 480 of a line that has no live
 * registration. The verdict belongs to call.c, which matches the
 * dialog and answers 400 for a header nobody can read, 481 for a dialog
 * nobody has, or 486 from the match itself; one 486 in front of that match
 * hides all three behind a single wrong answer.
 *
 * Presence is the whole test, and no part of the header is parsed here. This
 * layer keeps a dialog set of its own, but sip/dialog.h offers no lookup by
 * the (call-id, to-tag, from-tag) triple a Replaces names, and the filters
 * that turn a matched dialog into a replaceable leg live in call.c. A verdict
 * built here would therefore be a second opinion on call.c's rule.
 *
 * False for a response, for every other method, and for an INVITE that carries
 * no such header.
 */
bool acct_invite_replaces(const struct sip_msg *msg);

/* no line of the set carries the name */
#define ACCT_LINE_NONE		((unsigned int)-1)

/*
 * Which line a user name selects. users holds one contact user per line, NULL
 * for a line that is not active; sock_owner is the line whose socket the
 * datagram arrived on, or ACCT_LINE_NONE.
 *
 * One match wins, and an ambiguous name falls back to the socket owner. Two
 * lines can carry the same user (one subscriber account behind two ports is a
 * real deployment), and the first table entry is then an arbitrary choice: a
 * message summary would update the wrong line's store and light the wrong
 * phone. The socket a datagram arrived on is evidence that table order is not.
 * With no socket owner, or one that carries another name, the first match
 * stands.
 */
unsigned int acct_user_line(const char *const *users, unsigned int count,
			    struct sip_str user, unsigned int sock_owner);

/*
 * Which line a stream connection arrived on: the one whose local port, bind
 * device and address family the connection key names (sip/stream.h "The
 * connection key"). ports, ifaces and families hold one entry per line, port 0
 * or a NULL interface for a line that runs no stream. family is the family of
 * the key's peer, which the listener's IPV6_V6ONLY and the connect make the
 * family the line signals in, so two lines on one port and one device in two
 * families are two identities.
 *
 * The first match wins, which is the rule a shared UDP socket already follows:
 * the shipped two line configuration puts both lines on port 5060 and one
 * device, so they share every connection, and answering "no line" would drop
 * the last resort of the dispatch. A request that reaches that last resort over
 * a reliable transport gets no retransmission to save it (RFC 3261 17.2.4). The
 * answer is only the owner hint: an in-dialog request, a CANCEL and a named
 * user still overrule it in account_for_request(), exactly as they do for a
 * datagram, and the hint decides an ambiguous name alone.
 */
unsigned int acct_conn_line(const uint16_t *ports, const char *const *ifaces,
			    const int *families, unsigned int count,
			    uint16_t local_port, const char *iface, int family);

/*
 * Whether a changed resolved set must trigger a re-REGISTER. It is true only
 * when the DNS answer changed and dns_cache_update_mode selects it (mode 1),
 * which is the whole of the dns_cache_update_mode hook.
 */
bool acct_reregister_needed(bool dns_cache_update_mode, bool answer_changed);

/*
 * Whether a failed attempt has no fresh target left to advance to: the list
 * is empty, holds a single endpoint, or every endpoint sits in its penalty
 * box at now_ms. That is when a re-resolve ahead of the TTL can help and a
 * failover cannot. Exported on the acct_reregister_needed() precedent.
 */
bool acct_targets_exhausted(const struct sip_reg *reg, uint64_t now_ms);

/*
 * Whether a stream connection that reached SIP_CONN_UP for the registration
 * destination is a reconnect, which forces a REGISTER refresh (sip/stream.h,
 * the cb.conn paragraph): the registrar's binding names the ephemeral five
 * tuple of the connection that went away, so a refresh is what teaches it the
 * new one.
 *
 * up_seen is whether this registration ever saw one come up, known is the
 * handle it last saw and SIP_HANDLE_NONE after a close, and conn is the handle
 * that just came up. The first connection of a registration is therefore never
 * a reconnect, the same handle reported twice is not one either, and an up
 * after a close is. Exported so test_account.c drives the state machine
 * without a pool, the acct_reregister_needed() precedent.
 *
 * inbound is true for a connection that the peer opened (sip_conn.inbound).
 * Such a connection is never a reconnect: the peer that opened it already
 * reaches the line through it. A refresh would also drop the binding while the
 * request that the peer sent on the connection waits to be read. Over TCP the
 * binding can name such a connection, because a send takes any connection with
 * its key, an accepted one included (sip_stream_conn_find). When that
 * connection closes, a new one from the peer forces nothing, and the binding
 * waits for the next refresh or for a connection that this side opens.
 */
bool acct_reconnect_needed(bool up_seen, uint32_t known, uint32_t conn,
			   bool inbound);

/*
 * Whether a reconnect may force the REGISTER refresh right now. reg_state is
 * the enum sip_reg_state of the line, an int for the reason acct_status_name()
 * takes one.
 *
 * A registration with a request in flight does not refresh. A connection
 * reports up before its staged bytes drain, so the REGISTER that opened it is
 * still in flight when the upcall runs, and a stop and start would abandon that
 * transaction and put a second REGISTER on the wire. That in-flight request
 * already carries the five tuple of the new connection; one that travelled on
 * the connection that died ends on its own timer F and reg.c retries, so
 * nothing is lost by waiting either way.
 */
bool acct_reconnect_refresh_ok(int reg_state);

/*
 * The resolved registration targets of one line appended to the firewall proxy
 * source list, in the sorted order fw_reconcile_proxy_add() keeps
 * (net/fw_reconcile.h). Returns the new
 * count, so the next line collects on top of this one. started is the account's
 * reg_inited: a client that never started, or that a teardown stopped, keeps a
 * target list sip_reg_done() does not clear, and those addresses must not reach
 * a rule. Exported on the acct_targets_load() precedent, so the suite collects
 * what a scripted answer would put into a rule.
 */
unsigned int acct_proxy_collect(const struct sip_reg *reg, bool started,
				char (*list)[PROTO_FW_SRC_MAX],
				unsigned int count);

/*
 * The per-line mwi_subscription status key, emitted only when
 * the line subscribes and its client exists; the value is the sip/sub.h
 * status name verbatim. Exported so test_account.c pins the presence rule
 * without a live account.
 */
void acct_sub_status_dump(struct blob_buf *b, bool subscribe,
			  const struct sip_sub *sub);

/*
 * The transport keys of one line: the transport token every line carries, and
 * for a stream transport what its connection is doing. Presence is the message
 * again (acct_sub_status_dump): no "connection" table means the line holds no
 * connection at this moment, which is the normal state of a line between
 * registrations.
 *
 * conn is the connection serving the line's registration destination, or NULL.
 * listener says whether the line's inbound listener is up, and every stream
 * transport reports it: a TLS line because client only is a normal state
 * (sip/tls.h "Client only is a normal state"), where it registers and takes
 * no inbound connection until the certificate bundle lands, and a TCP line
 * because a listener that did not come back after an interface move is down
 * just as silently. tls_err is the connection error, else a client context
 * failure, and an empty one emits no key. Both reach the line level and not
 * the connection table, because a line with no connection is exactly where
 * they carry their news.
 *
 * Exported so test_account.c pins the presence rules without a live pool, the
 * acct_sub_status_dump() precedent.
 */
void acct_conn_status_dump(struct blob_buf *b, enum acct_transport transport,
			   const struct sip_conn *conn, bool listener,
			   const char *tls_err);

/*
 * A resolved answer applied to the sticky target list through the reload pair,
 * so the selection and the penalty state follow target identity across a DNS
 * TTL re-resolution (sip/reg.h). Returns whether the endpoint identity set
 * moved, the reload_end() report: the line's own change signal, read instead
 * of the resolver's cache flag, because a second line served from the shared
 * cache entry gets that flag false and would otherwise never react. Exported
 * so test_account.c can script two answers over one live registration.
 */
bool acct_targets_load(struct sip_reg *reg, const struct sip_dns_result *res);

/*
 * One resolver answer delivered to a live line, exactly as the resolver's own
 * completion would deliver it. Exported so test_account.c can place an answer
 * in a chosen frame of a running account: the resolver of a live daemon is out
 * of the suite's reach, so this is the only way to script a moved answer over
 * a live line. A line that is out of range or inactive is ignored.
 */
void acct_dns_result_inject(unsigned int line, const struct sip_dns_result *res);

/*
 * Test control of one line's resolution retry, never used by the daemon.
 * acct_test_dns_retry_ms() reads the time left on the resolve timer of the
 * line, so it reports the deadline in force and not the delay the ladder meant
 * to arm: an arm that records one delay and sets another is a defect a reader
 * of the record cannot see. It answers 0 for a line with no timer armed, and
 * for a timer that is already due. The caller allows for the time between the
 * arm and the read. acct_test_dns_retry_fire() runs a pending attempt the way
 * the uloop dispatch would, and answers false when none is armed. Both follow
 * the fw_test_retry_ms() precedent (fw.h).
 */
unsigned int acct_test_dns_retry_ms(unsigned int line);
bool acct_test_dns_retry_fire(unsigned int line);

/*
 * The resolver the account layer drives, read only. Two of its inputs are live
 * and come from this layer: the AAAA questions follow the operator gate, and
 * the family of the first delivered target follows the address netifd reports.
 * Neither appears in the status, so this is what pins them in the suite.
 */
const struct sip_dns *acct_dns(void);

/*
 * The datagram transport of the lines, read only. A line that moves family
 * opens a socket in the new family and releases the one it held, and the count
 * of the pool is where that shows; a send to a destination of another family
 * than the socket is refused and counted there too (sip/transport.h). Neither
 * reaches the status, so this is what pins them in the suite, the acct_dns()
 * precedent.
 */
const struct sip_transport *acct_transport(void);

/*
 * The same pool, writable, never used by the daemon. A case that has to watch
 * a rebind fail moves the configured port of a socket onto a port another
 * socket holds, the technique of test_sip_transport.c, and no read only view
 * allows it. Every other case reads the pool through acct_transport().
 */
struct sip_transport *acct_transport_test(void);

/* ------------------------------------------------------------- runtime */

/*
 * Build the transport and the resolver, and subscribe to the netifd tracker
 * (net/iface.h): the tracker reports every move of the voice interface, and
 * its recovery attempt reopens the sockets and listeners of this layer that a
 * failed rebind left down. Call once, before accounts_start().
 */
void accounts_init(void);

/*
 * The core services of the daemon (proto/proto.h struct proto_host): the netifd
 * tracker, the firewall reconcile, the conntrack bound, the registration
 * events, the TLS bundle fetch, the message waiting store and the trace. The
 * graceful stop reports its end through host->shutdown_done. Bound before
 * accounts_init().
 */
struct proto_host;
void accounts_host_bind(const struct proto_host *host);

/* Build every enabled account of cfg and start its registration. */
void accounts_start(const struct cfg *cfg);

/* Apply the difference between the running config and the new one. */
void accounts_reload(const struct cfg *old_cfg, const struct cfg *new_cfg);

/*
 * The backend feature mask (enum hal_feature), pushed by the shell beside
 * calls_hal_features_set() (call.h) and from the same answer. One bit reaches
 * this layer: HAL_FEAT_IPV6_MEDIA. A line registers over IPv6 only when the
 * backend carries IPv6 media, because the media of a call follows the family of
 * its signalling (RFC 6157 section 5), so a line that registered over IPv6 on a
 * backend without the bit would answer every call with congestion.
 *
 * A push after the lines are up moves the family the resolver delivers first.
 * A layer with no mask registers over IPv4 alone, which is what a suite that
 * drives the accounts directly gets.
 */
void accounts_hal_features_set(uint32_t features);

/*
 * Load the TLS contexts again from the certificate bundle on disk and close
 * every TLS connection built on the old one, whose session secrets belong to
 * the bundle that is going away (sip/tls.h sip_tls_ctx_load). A TLS listener
 * that stayed down for a missing certificate is retried here, which is what
 * turns a fetched bundle into an inbound service without a reload.
 *
 * The consumer is the certificate fetch (files/uvoice_tls_cert.sh, started
 * by event_tls_cert_start() in event.c): it calls this when the helper
 * reports a new bundle. It uses the tls_* values of the running
 * configuration, so it is a no-op before accounts_start() has seen one.
 */
void accounts_tls_reload(void);

/*
 * Begin the graceful stop: unregister every live account with Expires 0. The
 * caller then pumps uloop until accounts_shutdown_pending() clears or a grace
 * timeout fires, exactly as main.c waits on the HAL stop job.
 */
void accounts_shutdown_begin(void);
bool accounts_shutdown_pending(void);

/* Free the sockets and the resolver, and leave the tracker. */
void accounts_done(void);

/* Emit the lines array. A negative want_line includes all active lines; an
 * index includes that active line alone. */
void accounts_status_dump(struct blob_buf *b, int want_line);

/*
 * Where one line's registration stands, reported at the three edges that
 * report it: the answer that registered it, the unregister that took it down,
 * and the failure that lost it. The event layer decides for itself whether
 * the report is news for the cloud consumers, so every call reaches it.
 *
 * moved separates a state change from a refresh, which reports the state the
 * line already held. A registered answer arrives about every half expiry and
 * is a change only the first time; the other two edges are always changes.
 * An armed call trace asks for a snapshot on a change alone, because its
 * reason word names a move and the network table it renders answers one.
 *
 * Exported so test_account.c pins both consumers of one edge without a live
 * registrar: the three callbacks that call it are private and only run behind
 * a registration a suite cannot stage.
 */
void acct_reg_state_post(unsigned int line, bool up, const char *proxy,
			 bool moved);

/* Test control: (un)register one line by index. Returns false for a bad line. */
bool account_register_cmd(unsigned int line);
bool account_unregister_cmd(unsigned int line);

/*
 * --------------------------------------------- the call interface of account.c
 *
 * Owner namespace. One transaction manager and one env serve both the
 * registration client and the call legs of a line, so the opaque owner every
 * transaction carries gains a kind in its top nibble. reg.c stores the owner
 * without reading it, so account.c alone interprets it: it passes
 * acct_owner_make(ACCT_OWNER_REG, line + 1) to sip_reg_init() and routes the
 * env resp and done upcalls by acct_owner_kind(). The OWNER_CALL value is
 * the call handle, (index << 16) | generation, which fits the low 28 bits.
 */
enum acct_owner_kind {
	ACCT_OWNER_NONE = 0,
	ACCT_OWNER_REG,
	ACCT_OWNER_CALL,
	ACCT_OWNER_UA,
	ACCT_OWNER_KA,		/* keepalive.c OPTIONS ping transactions */
	ACCT_OWNER_SUB,		/* sub.c SUBSCRIBE transactions */
};

#define ACCT_OWNER_SHIFT	28

static inline uint32_t acct_owner_make(enum acct_owner_kind kind, uint32_t v)
{
	return ((uint32_t)kind << ACCT_OWNER_SHIFT) | v;
}

static inline enum acct_owner_kind acct_owner_kind_of(uint32_t owner)
{
	return (enum acct_owner_kind)(owner >> ACCT_OWNER_SHIFT);
}

static inline uint32_t acct_owner_val(uint32_t owner)
{
	return owner & ((1u << ACCT_OWNER_SHIFT) - 1);
}

struct sip_txn_mgr;
struct sip_txn_env;
struct sip_txn_dst;
struct sip_dialog_set;
struct ua;

/*
 * The per line view call.c works through. Pointers stay owned by the
 * account and are valid until the account is torn down or rebuilt;
 * calls_line_teardown() runs before either, so a leg never holds a stale
 * view. Strings follow the cfg lifetime rule: the leg copies what it keeps.
 * bind_dev has the shortest life of them: the next netifd answer or line
 * rebuild replaces it, so the leg copies it at once.
 * dst is the sticky registrar target of the moment; dst_valid is false
 * before the first resolution, and call_dial() then fails fast with the
 * congestion class. registered is true while the registration holds a live
 * binding (acct_reg_live); without it the seize gives the fast busy and
 * call_dial() fails the same way.
 * env is the one pointer a caller may keep past that lifetime: the account
 * table is static and the environment is the first member of both transport
 * arms, so one address stands for the life of the daemon and a rebuild writes
 * the same one back, which is why call.c holds it for the age of a refusal
 * (call.c struct call_refused).
 */
struct acct_call_view {
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct ua *ua;
	struct sip_dialog_set *dialogs;
	const char *local_host;
	const char *domain;
	const char *user;
	const char *display;
	const char *auth_user;
	const char *auth_pass;
	const char *user_agent;
	/*
	 * The transport token the leg puts in the Via and the Contact of the
	 * requests it builds. NULL takes UDP, and account_call_view() zeroes
	 * the whole view, so a line that never names a transport gets the UDP
	 * behaviour with no call site change. It is the uppercase form ("TCP",
	 * "TLS") that the Via carries after "SIP/2.0/"; the Contact writer
	 * lowercases it for ";transport=".
	 */
	const char *transport;
	/* the netdev the leg's media scopes to; NULL means unscoped */
	const char *bind_dev;
	const struct sip_txn_dst *dst;
	/*
	 * The service route the registrar granted (RFC 3608, sip/reg.h), or
	 * NULL when the request must carry none. The registrar granted it with
	 * the binding it holds. The view therefore exposes it only while dst
	 * still names the server that granted it: a Route for one SBC must not
	 * reach an INVITE to another, which granted no such route. The leg
	 * copies the block and does not keep this pointer: every retry of one
	 * INVITE carries the route the first one carried.
	 */
	const struct sip_route_hdrs *service_route;
	/*
	 * The cadence of the media punch the leg hands the backend
	 * (hal.h, hal_media.punch_interval_ms): half the conntrack udp bound
	 * of the moment, capped at the RFC 6263 section 7 interval. The leg
	 * copies it beside bind_dev, so a bound that moves reaches the next
	 * leg and never a running stream.
	 */
	uint16_t punch_interval_ms;
	uint16_t local_port;
	bool dst_valid;
	bool registered;
	bool active;
};

/* False for a bad line or an account that is not built. */
bool account_call_view(unsigned int line, struct acct_call_view *out);

#endif
