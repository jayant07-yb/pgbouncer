/*
 * PgBouncer - Lightweight connection pooler for PostgreSQL.
 *
 * Copyright (c) 2007-2009  Marko Kreen, Skype Technologies OÜ
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Client connection handling
 */

#include "bouncer.h"
#include "pam.h"
#include "scram.h"

#include <usual/pgutil.h>

static const char *hdr2hex(const struct MBuf *data, char *buf, unsigned buflen)
{
	const uint8_t *bin = data->data + data->read_pos;
	unsigned int dlen;

	dlen = mbuf_avail_for_read(data);
	return bin2hex(bin, dlen, buf, buflen);
}

static bool check_client_passwd(PgSocket *client, const char *passwd)
{
	PgUser *user = client->login_user;
	int auth_type = client->client_auth_type;

	if (user->mock_auth)
		return false;

	/* disallow empty passwords */
	if (!*user->passwd)
		return false;

	switch (auth_type) {
	case AUTH_PLAIN:
		switch (get_password_type(user->passwd)) {
		case PASSWORD_TYPE_PLAINTEXT:
			return strcmp(user->passwd, passwd) == 0;
		case PASSWORD_TYPE_MD5: {
			char md5[MD5_PASSWD_LEN + 1];
			pg_md5_encrypt(passwd, user->name, strlen(user->name), md5);
			return strcmp(user->passwd, md5) == 0;
		}
		case PASSWORD_TYPE_SCRAM_SHA_256:
			return scram_verify_plain_password(client, user->name, passwd, user->passwd);
		default:
			return false;
		}
	case AUTH_MD5: {
		char *stored_passwd;
		char md5[MD5_PASSWD_LEN + 1];

		if (strlen(passwd) != MD5_PASSWD_LEN)
			return false;

		/*
		 * The client sends
		 * 'md5'+md5(md5(password+username)+salt).  The stored
		 * password is either 'md5'+md5(password+username) or
		 * plain text.  If the latter, we compute the inner
		 * md5() call first.
		 */
		if (get_password_type(user->passwd) == PASSWORD_TYPE_PLAINTEXT) {
			pg_md5_encrypt(user->passwd, user->name, strlen(user->name), md5);
			stored_passwd = md5;
		} else {
			stored_passwd = user->passwd;
		}
		pg_md5_encrypt(stored_passwd + 3, (char *)client->tmp_login_salt, 4, md5);
		return strcmp(md5, passwd) == 0;
	}
	}
	return false;
}

static bool send_client_authreq(PgSocket *client)
{
	int res;
	int auth_type = client->client_auth_type;

	if (auth_type == AUTH_MD5) {
		uint8_t saltlen = 4;
		get_random_bytes((void*)client->tmp_login_salt, saltlen);
		SEND_generic(res, client, 'R', "ib", AUTH_MD5, client->tmp_login_salt, saltlen);
	} else if (auth_type == AUTH_PLAIN || auth_type == AUTH_PAM) {
		SEND_generic(res, client, 'R', "i", AUTH_PLAIN);
	} else if (auth_type == AUTH_SCRAM_SHA_256) {
		SEND_generic(res, client, 'R', "iss", AUTH_SASL, "SCRAM-SHA-256", "");
	} else {
		return false;
	}

	if (!res)
		disconnect_client(client, false, "failed to send auth req");
	return res;
}

static void start_auth_query(PgSocket *client, const char *username)
{
	int res;
	PktBuf *buf;

	/* have to fetch user info from db */
	client->pool = get_pool(client->db, client->db->auth_user);
	if (!find_server(client)) {
		client->wait_for_user_conn = true;
		return;
	}
	slog_noise(client, "doing auth_conn query");
	client->wait_for_user_conn = false;
	client->wait_for_user = true;
	if (!sbuf_pause(&client->sbuf)) {
		release_server(client->link);
		disconnect_client(client, true, "pause failed");
		return;
	}
	client->link->ready = false;

	res = 0;
	buf = pktbuf_dynamic(512);
	if (buf) {
		pktbuf_write_ExtQuery(buf, cf_auth_query, 1, username);
		res = pktbuf_send_immediate(buf, client->link);
		pktbuf_free(buf);
		/*
		 * Should do instead:
		 *   res = pktbuf_send_queued(buf, client->link);
		 * but that needs better integration with SBuf.
		 */
	}
	if (!res)
		disconnect_server(client->link, false, "unable to send auth_query");
	client->expect_rfq_count++;
}

static bool login_via_cert(PgSocket *client)
{
	struct tls *tls = client->sbuf.tls;

	if (!tls) {
		slog_error(client, "TLS connection required");
		goto fail;
	}
	if (!tls_peer_cert_provided(client->sbuf.tls)) {
		slog_error(client, "TLS client certificate required");
		goto fail;
	}
	if (client->login_user->mock_auth)
		goto fail;

	log_debug("TLS cert login: %s", tls_peer_cert_subject(client->sbuf.tls));
	if (!tls_peer_cert_contains_name(client->sbuf.tls, client->login_user->name)) {
		slog_error(client, "TLS certificate name mismatch");
		goto fail;
	}

	/* login successful */
	return finish_client_login(client);
fail:
	disconnect_client(client, true, "certificate authentication failed");
	return false;
}

static bool login_as_unix_peer(PgSocket *client)
{
	if (!pga_is_unix(&client->remote_addr))
		goto fail;
	if (client->login_user->mock_auth)
		goto fail;
	if (!check_unix_peer_name(sbuf_socket(&client->sbuf), client->login_user->name))
		goto fail;
	return finish_client_login(client);
fail:
	disconnect_client(client, true, "unix socket login rejected");
	return false;
}

static bool finish_set_pool(PgSocket *client, bool takeover)
{
	bool ok = false;
	int auth;

	if (!client->login_user->mock_auth && !client->db->fake) {
		PgUser *pool_user;

		if (client->db->forced_user)
			pool_user = client->db->forced_user;
		else
			pool_user = client->login_user;

		client->pool = get_pool(client->db, pool_user);
		if (!client->pool) {
			disconnect_client(client, true, "no memory for pool");
			return false;
		}
	}

	if (cf_log_connections) {
		if (client->sbuf.tls) {
			char infobuf[96] = "";
			tls_get_connection_info(client->sbuf.tls, infobuf, sizeof infobuf);
			slog_info(client, "login attempt: db=%s user=%s tls=%s",
				  client->db->name, client->login_user->name, infobuf);
		} else {
			slog_info(client, "login attempt: db=%s user=%s tls=no",
				  client->db->name, client->login_user->name);
		}
	}

	if (takeover)
		return true;

	if (client->pool && client->pool->db->admin) {
		if (!admin_post_login(client))
			return false;
	}

	if (client->own_user)
		return finish_client_login(client);

	auth = cf_auth_type;
	if (auth == AUTH_HBA) {
		auth = hba_eval(parsed_hba, &client->remote_addr, !!client->sbuf.tls,
				client->db->name, client->login_user->name);
	}

	if (auth == AUTH_MD5)
	{
		if (get_password_type(client->login_user->passwd) == PASSWORD_TYPE_SCRAM_SHA_256)
			auth = AUTH_SCRAM_SHA_256;
	}

	/* remember method */
	client->client_auth_type = auth;

	switch (auth) {
	case AUTH_ANY:
		ok = finish_client_login(client);
		break;
	case AUTH_TRUST:
		if (client->login_user->mock_auth)
			disconnect_client(client, true, "\"trust\" authentication failed");
		else
			ok = finish_client_login(client);
		break;
	case AUTH_PLAIN:
	case AUTH_MD5:
	case AUTH_PAM:
	case AUTH_SCRAM_SHA_256:
		ok = send_client_authreq(client);
		break;
	case AUTH_CERT:
		ok = login_via_cert(client);
		break;
	case AUTH_PEER:
		ok = login_as_unix_peer(client);
		break;
	default:
		disconnect_client(client, true, "login rejected");
		ok = false;
	}
	return ok;
}

bool set_pool(PgSocket *client, const char *dbname, const char *username, const char *password, bool takeover)
{
	Assert((password && takeover) || (!password && !takeover));

	/* find database */
	client->db = find_database(dbname);
	if (!client->db) {
		client->db = register_auto_database(dbname);
		if (client->db)
			slog_info(client, "registered new auto-database: db=%s", dbname);
	}
	if (!client->db) {
		client->db = calloc(1, sizeof(*client->db));
		client->db->fake = true;
		strlcpy(client->db->name, dbname, sizeof(client->db->name));
	}

	if (client->db->admin) {
		if (admin_pre_login(client, username))
			return finish_set_pool(client, takeover);
	}

	/* avoid dealing with invalid data below, and give an
	 * appropriate error message */
	if (strlen(username) >= MAX_USERNAME) {
		disconnect_client(client, true, "username too long");
		if (cf_log_connections)
			slog_info(client, "login failed: db=%s user=%s", dbname, username);
		return false;
	}
	if (password && strlen(password) >= MAX_PASSWORD) {
		disconnect_client(client, true, "password too long");
		if (cf_log_connections)
			slog_info(client, "login failed: db=%s user=%s", dbname, username);
		return false;
	}

	/* find user */
	if (cf_auth_type == AUTH_ANY) {
		/* ignore requested user */
		if (client->db->forced_user == NULL) {
			slog_error(client, "auth_type=any requires forced user");
			disconnect_client(client, true, "bouncer config error");
			return false;
		}
		client->login_user = client->db->forced_user;
	} else if (cf_auth_type == AUTH_PAM) {
		if (client->db->auth_user) {
			slog_error(client, "PAM can't be used together with database authentication");
			disconnect_client(client, true, "bouncer config error");
			return false;
		}
		/* Password will be set after successful authentication when not in takeover mode */
		client->login_user = add_pam_user(username, password);
		if (!client->login_user) {
			slog_error(client, "set_pool(): failed to allocate new PAM user");
			disconnect_client(client, true, "bouncer resources exhaustion");
			return false;
		}
	} else {
		client->login_user = find_user(username);
		if (!client->login_user) {
			/*
			 * If the login user specified by the client
			 * does not exist, check if an auth_user is
			 * set and if so send off an auth_query.  If
			 * no auth_user is set for the db, see if the
			 * global auth_user is set and use that.
			 */
			if (!client->db->auth_user && cf_auth_user) {
				client->db->auth_user = find_user(cf_auth_user);
				if (!client->db->auth_user)
					client->db->auth_user = add_user(cf_auth_user, "");
			}
			if (client->db->auth_user) {
				if (client->db->fake)
					slog_debug(client, "not running auth_query because database is fake");
				else {
					if (takeover) {
						client->login_user = add_db_user(client->db, username, password);
						return finish_set_pool(client, takeover);
					}
					start_auth_query(client, username);
					return false;
				}
			}

			slog_info(client, "no such user: %s", username);
			client->login_user = calloc(1, sizeof(*client->login_user));
			client->login_user->mock_auth = true;
			safe_strcpy(client->login_user->name, username, sizeof(client->login_user->name));
		}
	}

	/* if a pool is assigned to the client connection then assign the client_id */
	if(finish_set_pool(client,takeover))
		{
			
			client->client_id = ++(client->pool->client_counter );
			return true;
		}
	return false ;
}

bool handle_auth_query_response(PgSocket *client, PktHdr *pkt) {
	uint16_t columns;
	uint32_t length;
	const char *username, *password;
	PgUser user;
	PgSocket *server = client->link;

	switch(pkt->type) {
	case 'T':	/* RowDescription */
		if (!mbuf_get_uint16be(&pkt->data, &columns)) {
			disconnect_server(server, false, "bad packet");
			return false;
		}
		if (columns != 2u) {
			disconnect_server(server, false, "expected 2 columns from auth_query, not %hu", columns);
			return false;
		}
		break;
	case 'D':	/* DataRow */
		memset(&user, 0, sizeof(user));
		if (!mbuf_get_uint16be(&pkt->data, &columns)) {
			disconnect_server(server, false, "bad packet");
			return false;
		}
		if (columns != 2u) {
			disconnect_server(server, false, "expected 2 columns from auth_query, not %hu", columns);
			return false;
		}
		if (!mbuf_get_uint32be(&pkt->data, &length)) {
			disconnect_server(server, false, "bad packet");
			return false;
		}
		if (length == (uint32_t)-1) {
			disconnect_server(server, false, "auth_query response contained null user name");
			return false;
		}
		if (!mbuf_get_chars(&pkt->data, length, &username)) {
			disconnect_server(server, false, "bad packet");
			return false;
		}
		if (sizeof(user.name) - 1 < length)
			length = sizeof(user.name) - 1;
		memcpy(user.name, username, length);
		if (!mbuf_get_uint32be(&pkt->data, &length)) {
			disconnect_server(server, false, "bad packet");
			return false;
		}
		if (length == (uint32_t)-1) {
			/*
			 * NULL - set an md5 password with an impossible value,
			 * so that nothing will ever match
			 */
			password = "md5";
			length = 3;
		} else {
			if (!mbuf_get_chars(&pkt->data, length, &password)) {
				disconnect_server(server, false, "bad packet");
				return false;
			}
		}
		if (sizeof(user.passwd)  - 1 < length)
			length = sizeof(user.passwd) - 1;
		memcpy(user.passwd, password, length);

		client->login_user = add_db_user(client->db, user.name, user.passwd);
		if (!client->login_user) {
			disconnect_server(server, false, "unable to allocate new user for auth");
			return false;
		}
		break;
	case 'N':	/* NoticeResponse */
		break;
	case 'C':	/* CommandComplete */
		break;
	case '1':	/* ParseComplete */
		break;
	case '2':	/* BindComplete */
		break;
	case 'S': /* ParameterStatus */
		break;
	case 'Z':	/* ReadyForQuery */
		sbuf_prepare_skip(&client->link->sbuf, pkt->len);
		if (!client->login_user) {
			if (cf_log_connections)
				slog_info(client, "login failed: db=%s", client->db->name);
			/*
			 * TODO: Currently no mock authentication when
			 * using auth_query/auth_user; we just abort
			 * with a revealing message to the client.
			 * The main problem is that at this point we
			 * don't know the original user name anymore
			 * to do that.  As a workaround, the
			 * auth_query could be written in a way that
			 * it returns a fake user and password if the
			 * requested user doesn't exist.
			 */
			disconnect_client(client, true, "no such user");
		} else {
			slog_noise(client, "auth query complete");
			client->link->resetting = true;
			sbuf_continue(&client->sbuf);
		}
		/*
		 * either sbuf_continue or disconnect_client could disconnect the server
		 * way down in their bowels of other callbacks. so check that, and
		 * return appropriately (similar to reuse_on_release)
		 */
		if (server->state == SV_FREE || server->state == SV_JUSTFREE)
			return false;
		return true;
	case 'E':	/* ErrorResponse */
		disconnect_server(server, false, "error response from auth_query");
		return false;
	default:
		disconnect_server(server, false, "unexpected response from auth_query");
		return false;
	}
	sbuf_prepare_skip(&server->sbuf, pkt->len);
	return true;
}

static void set_appname(PgSocket *client, const char *app_name)
{
	char buf[400], abuf[300];
	const char *details;

	if (cf_application_name_add_host) {
		/* give app a name */
		if (!app_name)
			app_name = "app";

		/* add details */
		details = pga_details(&client->remote_addr, abuf, sizeof(abuf));
		snprintf(buf, sizeof(buf), "%s - %s", app_name, details);
		app_name = buf;
	}
	if (app_name) {
		slog_debug(client, "using application_name: %s", app_name);
		varcache_set(&client->vars, "application_name", app_name);
	}
}

static bool decide_startup_pool(PgSocket *client, PktHdr *pkt)
{
	const char *username = NULL, *dbname = NULL;
	const char *key, *val;
	bool ok;
	bool appname_found = false;

	while (1) {
		ok = mbuf_get_string(&pkt->data, &key);
		if (!ok || *key == 0)
			break;
		ok = mbuf_get_string(&pkt->data, &val);
		if (!ok)
			break;

		if (strcmp(key, "database") == 0) {
			slog_debug(client, "got var: %s=%s", key, val);
			dbname = val;
		} else if (strcmp(key, "user") == 0) {
			slog_debug(client, "got var: %s=%s", key, val);
			username = val;
		} else if (strcmp(key, "application_name") == 0) {
			set_appname(client, val);
			appname_found = true;
		} else if (varcache_set(&client->vars, key, val)) {
			slog_debug(client, "got var: %s=%s", key, val);
		} else if (strlist_contains(cf_ignore_startup_params, key)) {
			slog_debug(client, "ignoring startup parameter: %s=%s", key, val);
		} else {
			slog_warning(client, "unsupported startup parameter: %s=%s", key, val);
			disconnect_client(client, true, "unsupported startup parameter: %s", key);
			return false;
		}
	}
	if (!username || !username[0]) {
		disconnect_client(client, true, "no username supplied");
		return false;
	}

	/* if missing dbname, default to username */
	if (!dbname || !dbname[0])
		dbname = username;

	/* create application_name if requested */
	if (!appname_found)
		set_appname(client, NULL);

	/* check if limit allows, don't limit admin db
	   nb: new incoming conn will be attached to PgSocket, thus
	   get_active_client_count() counts it */
	if (get_active_client_count() > cf_max_client_conn) {
		if (strcmp(dbname, "pgbouncer") != 0) {
			disconnect_client(client, true, "no more connections allowed (max_client_conn)");
			return false;
		}
	}

	/* find pool */
	return set_pool(client, dbname, username, NULL, false);
}

static bool scram_client_first(PgSocket *client, uint32_t datalen, const uint8_t *data)
{
	char *ibuf;
	char *input;
	int res;
	PgUser *user = client->login_user;

	ibuf = malloc(datalen + 1);
	if (ibuf == NULL)
		return false;
	memcpy(ibuf, data, datalen);
	ibuf[datalen] = '\0';

	input = ibuf;
	slog_debug(client, "SCRAM client-first-message = \"%s\"", input);
	if (!read_client_first_message(client, input,
				       &client->scram_state.cbind_flag,
				       &client->scram_state.client_first_message_bare,
				       &client->scram_state.client_nonce))
		goto failed;

	if (!user->mock_auth) {
		slog_debug(client, "stored secret = \"%s\"", user->passwd);
		switch (get_password_type(user->passwd)) {
		case PASSWORD_TYPE_MD5:
			slog_error(client, "SCRAM authentication failed: user has MD5 secret");
			goto failed;
		case PASSWORD_TYPE_PLAINTEXT:
		case PASSWORD_TYPE_SCRAM_SHA_256:
			break;
		}
	}

	if (!build_server_first_message(&client->scram_state, user->name, user->mock_auth ? NULL : user->passwd))
		goto failed;
	slog_debug(client, "SCRAM server-first-message = \"%s\"", client->scram_state.server_first_message);

	SEND_generic(res, client, 'R', "ib",
		     AUTH_SASL_CONT,
		     client->scram_state.server_first_message,
		     strlen(client->scram_state.server_first_message));

	free(ibuf);
	return res;
failed:
	free(ibuf);
	return false;
}

static bool scram_client_final(PgSocket *client, uint32_t datalen, const uint8_t *data)
{
	char *ibuf;
	char *input;
	const char *client_final_nonce = NULL;
	char *proof = NULL;
	char *server_final_message;
	int res;

	ibuf = malloc(datalen + 1);
	if (ibuf == NULL)
		return false;
	memcpy(ibuf, data, datalen);
	ibuf[datalen] = '\0';

	input = ibuf;
	slog_debug(client, "SCRAM client-final-message = \"%s\"", input);
	if (!read_client_final_message(client, data, input,
				       &client_final_nonce,
				       &proof))
		goto failed;
	slog_debug(client, "SCRAM client-final-message-without-proof = \"%s\"",
		   client->scram_state.client_final_message_without_proof);

	if (!verify_final_nonce(&client->scram_state, client_final_nonce)) {
		slog_error(client, "invalid SCRAM response (nonce does not match)");
		goto failed;
	}

	if (!verify_client_proof(&client->scram_state, proof)
	    || !client->login_user) {
		slog_error(client, "password authentication failed");
		goto failed;
	}

	server_final_message = build_server_final_message(&client->scram_state);
	if (!server_final_message)
		goto failed;
	slog_debug(client, "SCRAM server-final-message = \"%s\"", server_final_message);

	SEND_generic(res, client, 'R', "ib",
		     AUTH_SASL_FIN,
		     server_final_message,
		     strlen(server_final_message));

	free(server_final_message);
	free(proof);
	free(ibuf);
	return res;
failed:
	free(proof);
	free(ibuf);
	return false;
}

/* decide on packets of client in login phase */
static bool handle_client_startup(PgSocket *client, PktHdr *pkt)
{
	const char *passwd;
	const uint8_t *key;
	bool ok;
	bool is_unix = pga_is_unix(&client->remote_addr);

	SBuf *sbuf = &client->sbuf;

	/* don't tolerate partial packets */
	if (incomplete_pkt(pkt)) {
		disconnect_client(client, true, "client sent partial pkt in startup phase");
		return false;
	}

	if (client->wait_for_welcome || client->wait_for_auth) {
		if  (finish_client_login(client)) {
			/* the packet was already parsed */
			sbuf_prepare_skip(sbuf, pkt->len);
			return true;
		} else {
			return false;
		}
	}

	switch (pkt->type) {
	case PKT_SSLREQ:
		slog_noise(client, "C: req SSL");

		if (client->sbuf.tls) {
			disconnect_client(client, false, "SSL req inside SSL");
			return false;
		}
		if (client_accept_sslmode != SSLMODE_DISABLED && !is_unix) {
			slog_noise(client, "P: SSL ack");
			if (!sbuf_answer(&client->sbuf, "S", 1)) {
				disconnect_client(client, false, "failed to ack SSL");
				return false;
			}
			if (!sbuf_tls_accept(&client->sbuf)) {
				disconnect_client(client, false, "failed to accept SSL");
				return false;
			}
			break;
		}

		/* reject SSL attempt */
		slog_noise(client, "P: nak");
		if (!sbuf_answer(&client->sbuf, "N", 1)) {
			disconnect_client(client, false, "failed to nak SSL");
			return false;
		}
		break;
	case PKT_GSSENCREQ:
		/* reject GSS encryption attempt */
		slog_noise(client, "C: req GSS enc");
		if (!sbuf_answer(&client->sbuf, "N", 1)) {
			disconnect_client(client, false, "failed to nak GSS enc");
			return false;
		}
		break;
	case PKT_STARTUP_V2:
		disconnect_client(client, true, "old V2 protocol not supported");
		return false;
	case PKT_STARTUP:
		/* require SSL except on unix socket */
		if (client_accept_sslmode >= SSLMODE_REQUIRE && !client->sbuf.tls && !is_unix) {
			disconnect_client(client, true, "SSL required");
			return false;
		}

		if (client->pool && !client->wait_for_user_conn && !client->wait_for_user) {
			disconnect_client(client, true, "client re-sent startup pkt");
			return false;
		}

		if (client->wait_for_user) {
			client->wait_for_user = false;
			if (!finish_set_pool(client, false))
				return false;
		} else if (!decide_startup_pool(client, pkt)) {
			return false;
		}

		break;
	case 'p':		/* PasswordMessage, SASLInitialResponse, or SASLResponse */
		/* too early */
		if (!client->login_user) {
			disconnect_client(client, true, "client password pkt before startup packet");
			return false;
		}

		if (client->client_auth_type == AUTH_SCRAM_SHA_256) {
			const char *mech;
			uint32_t length;
			const uint8_t *data;

			if (!client->scram_state.server_nonce) {
				/* process as SASLInitialResponse */
				if (!mbuf_get_string(&pkt->data, &mech))
					return false;
				slog_debug(client, "C: selected SASL mechanism: %s", mech);
				if (strcmp(mech, "SCRAM-SHA-256") != 0) {
					disconnect_client(client, true, "client selected an invalid SASL authentication mechanism");
					return false;
				}
				if (!mbuf_get_uint32be(&pkt->data, &length))
					return false;
				if (!mbuf_get_bytes(&pkt->data, length, &data))
					return false;
				if (!scram_client_first(client, length, data)) {
					disconnect_client(client, true, "SASL authentication failed");
					return false;
				}
			} else {
				/* process as SASLResponse */
				length = mbuf_avail_for_read(&pkt->data);
				if (!mbuf_get_bytes(&pkt->data, length, &data))
					return false;
				if (scram_client_final(client, length, data)) {
					/* save SCRAM keys for user */
					if (!client->scram_state.adhoc) {
						memcpy(client->pool->user->scram_ClientKey,
						       client->scram_state.ClientKey,
						       sizeof(client->scram_state.ClientKey));
						memcpy(client->pool->user->scram_ServerKey,
						       client->scram_state.ServerKey,
						       sizeof(client->scram_state.ServerKey));
						client->pool->user->has_scram_keys = true;
					}

					free_scram_state(&client->scram_state);
					if (!finish_client_login(client))
						return false;
				}
				else {
					disconnect_client(client, true, "SASL authentication failed");
					return false;
				}
			}
		} else {
			/* process as PasswordMessage */
			ok = mbuf_get_string(&pkt->data, &passwd);

			if (ok) {
				/*
				 * Don't allow an empty password; see
				 * PostgreSQL recv_password_packet().
				 */
				if (!*passwd) {
					disconnect_client(client, true, "empty password returned by client");
					return false;
				}

				if (client->client_auth_type == AUTH_PAM) {
					if (!sbuf_pause(&client->sbuf)) {
						disconnect_client(client, true, "pause failed");
						return false;
					}
					pam_auth_begin(client, passwd);
					return false;
				}

				if (check_client_passwd(client, passwd)) {
					if (!finish_client_login(client))
						return false;
				} else {
					disconnect_client(client, true, "password authentication failed");
					return false;
				}
			}
		}
		break;
	case PKT_CANCEL:
		if (mbuf_avail_for_read(&pkt->data) == BACKENDKEY_LEN
		    && mbuf_get_bytes(&pkt->data, BACKENDKEY_LEN, &key))
		{
			memcpy(client->cancel_key, key, BACKENDKEY_LEN);
			accept_cancel_request(client);
		} else {
			disconnect_client(client, false, "bad cancel request");
		}
		return false;
	default:
		disconnect_client(client, false, "bad packet");
		return false;
	}
	sbuf_prepare_skip(sbuf, pkt->len);
	client->request_time = get_cached_time();
	return true;
}

/*
 * Yugabyte Changes 
 */

#define CLIENTID_SIZE 3
#define STMTNAME_START_PARSE 5 
#define PORTALNAME_START_BIND 5
#define REGISTER_PSMT_IGNORE 0
#define BIND_PKT_PSMT_IGNORE 1

#define MEMORY_ERROR "Unable to allocate memory	!!"

const char client_id_map[] = {'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6','7','8','9','_'};


PktBuf *shared_buf;
PktBuf *bind_pkt_buf ; 

char* get_stmt_name(int client_id, const char* data , int startpoint)
{
	int len =strlen(data+startpoint);

	char *server_side_prepare_statement_id =(char *)malloc(sizeof(char)*(len+2+ CLIENTID_SIZE)) ;
	
	if(!server_side_prepare_statement_id)
		log_error(MEMORY_ERROR);

	for(int i=0;i<len+5 ;i++ )
		server_side_prepare_statement_id[i] =0;

	server_side_prepare_statement_id[0] = client_id_map[client_id/(36*36)] ;
	client_id %= 36*36 ;
	server_side_prepare_statement_id[1] = client_id_map[client_id/(36)] ;
	client_id %= 36 ; 
	server_side_prepare_statement_id[2] = client_id_map[client_id] ;
	
	strcpy(server_side_prepare_statement_id + CLIENTID_SIZE,data+startpoint);
	
	return  server_side_prepare_statement_id ;
}

struct prepStmt* create_prep_stmt_obj(const PgSocket *client, const PktHdr *pkt){
	struct prepStmt *result = (struct prepStmt *)malloc(sizeof(struct prepStmt)) ; 
	if(!result)
		log_error(MEMORY_ERROR);

	result->server_side_prepare_statement_id = get_stmt_name(client->client_id , pkt->data.data , STMTNAME_START_PARSE) ; 
	result->parse_packet = (uint8_t *)malloc(sizeof(uint8_t )*pkt->len);
	
	/*	We need to copy the packet this way only since it contains end of the packet command several times */
	for(int i=0;i<pkt->len;i++)
		result->parse_packet[i] = pkt->data.data[i];

	return result ; 
}

void register_parse_packet(PgSocket *client, PktHdr *pkt)
{
	/* Unnamed prepared statements will be forwarded without any changes */
	if (pkt->data.data[STMTNAME_START_PARSE]==NULL)		
		return; 						

	struct prepStmt *ppstmt_client_copy , *ppstmt_server_copy ; 

	ppstmt_client_copy = create_prep_stmt_obj(client, pkt);
	ppstmt_server_copy = create_prep_stmt_obj(client, pkt);
	assert(strcmp(ppstmt_client_copy->server_side_prepare_statement_id,ppstmt_server_copy->server_side_prepare_statement_id)==0);

	if(!ppstmt_client_copy || !ppstmt_server_copy)
		log_error(MEMORY_ERROR);

	/*	Add the preparedStatement to client list */

	aatree_insert(&(client->stmt_tree), (uintptr_t)ppstmt_client_copy->server_side_prepare_statement_id, &ppstmt_client_copy->tree_node);
	aatree_insert(&(client->link->stmt_tree), (uintptr_t)ppstmt_server_copy->server_side_prepare_statement_id, &ppstmt_server_copy->tree_node);

	/* Prepare the new Packet */
	send_parse_pkt(client->link,ppstmt_server_copy,REGISTER_PSMT_IGNORE);
}

//Call it with the server list 
void send_parse_pkt(PgSocket *server, struct prepStmt *ppstmt, bool serverIgnore)
{	
	bool res;
	PktBuf *buf = pktbuf_dynamic(512);

	/* Parse */
	pktbuf_start_packet(buf, 'P');

	/* Send the two chars of the client ID	*/
	int client_id=server->link->client_id ;
	pktbuf_put_char(buf,client_id_map[client_id/(36*36)] );
	client_id %= 36*36 ;
	pktbuf_put_char(buf,client_id_map[client_id/(36)]);
	client_id %= 36 ; 
	pktbuf_put_char(buf,client_id_map[client_id]); ;

	/* Need to change the size */
	for(int i=5;i<ppstmt->parse_packet_len;i++)
		pktbuf_put_char(buf,ppstmt->parse_packet[i]);

	pktbuf_finish_packet(buf);

	res = pktbuf_send_immediate(buf, server);

	if(res && serverIgnore)
		server->skip_pkt_1++;

	pktbuf_free(buf);
}

void copy_ppstmt_objs(struct prepStmt *dest,  struct prepStmt *src)
{
	/* We need to copy everything except the tree node */
	dest->parse_packet_len = src->parse_packet_len ;
	int stmtlen = strlen(src->server_side_prepare_statement_id) ;
	dest->server_side_prepare_statement_id = (char * )malloc(sizeof(char) * (1+stmtlen));
	if(!dest->server_side_prepare_statement_id)
		log_error(MEMORY_ERROR);
	strcpy(dest->server_side_prepare_statement_id , src->server_side_prepare_statement_id);

	dest->parse_packet = (char * )malloc(sizeof(char)*(dest->parse_packet_len));
	for(int i=0 ;i<dest->parse_packet_len ;i++)
		dest->parse_packet[i]  =  src->parse_packet[i];
}

int starting_point_bind_pkt(const PktHdr *pkt)
{
	int ending_point_portal_name = 5 ; 
	int starting_point_prepared_stmt = ending_point_portal_name+1 ;
	for( ; pkt->data.data[ending_point_portal_name] != 0 ; ending_point_portal_name++ , starting_point_prepared_stmt++) ;
	return starting_point_prepared_stmt;
}

void create_if_not_exist_ppstmt(PgSocket *server,  PktHdr *pkt)
{
	/* 
	 * Structure of the Bind packet 
	 * 
	 * |	B	|	LENGTH	|	PORTALNAME	|	PREPARED STATEMENT NAME	|	VALUES .... |
	 * Unnamed prepared statements are not to be processed 
	 * 
	*/
	
	int prepget_stmt_namestart  = starting_point_bind_pkt(pkt);

	/* Unnamed prepared statments will be forwarded unchanged */
	if( pkt->data.data[prepget_stmt_namestart]==0)
		return ;

	assert(pkt->type == 'B');

	char *name = get_stmt_name(server->link->client_id , pkt->data.data , prepget_stmt_namestart) ; 
	
	/* Prepared statement is prepared in the server connection*/
	if(aatree_search(&server->stmt_tree, (uintptr_t)name))
		return ;

	/* Is the prepared statement prepared by the client on any other server connection */
	struct AANode *node;
	node = aatree_search(&server->link->stmt_tree, (uintptr_t)name);
	struct prepStmt* ClientCopy = node ? container_of(node, struct prepStmt , tree_node) : NULL;
	if(ClientCopy == NULL) 
	{
		slog_debug(NULL, "Not found");
		return ;
	}

	/* We need to prepare the statement on the current server connection */
	struct prepStmt* ServerCopy = (struct prepStmt *)malloc(sizeof(struct prepStmt)) ; 
	copy_ppstmt_objs(ServerCopy, ClientCopy);
	assert(strcmp(ServerCopy->server_side_prepare_statement_id ,ClientCopy->server_side_prepare_statement_id)==0 );

	aatree_insert(&server->stmt_tree, (uintptr_t)name, &ServerCopy->tree_node);
	send_parse_pkt(server,ServerCopy,BIND_PKT_PSMT_IGNORE);
}

void send_immediate_one_pkt(PgSocket *client, struct PktHdr *pkt)
{
	if(!shared_buf)
		shared_buf = pktbuf_dynamic(512);
	
	PktBuf *buf = shared_buf;

	pktbuf_start_packet(buf,*pkt->data.data);
		pktbuf_put_bytes(buf,pkt->data.data+5,pkt->len-5);
		pktbuf_finish_packet(buf);
	
	
	bool res = pktbuf_send_immediate(buf,client->link);
	sbuf_prepare_skip(&client->sbuf,pkt->len);
	pktbuf_reset(buf);
}

void send_bind_pkt(PgSocket *client, struct PktHdr *pkt , int startingpoint)
{
	bool res;
	if(!bind_pkt_buf) 
	 	bind_pkt_buf = pktbuf_dynamic(512);
		
	PktBuf *buf = bind_pkt_buf;

	/* Bind */
	pktbuf_start_packet(buf, 'B');
	
	/* Need to change the size */
	
	for(int i=PORTALNAME_START_BIND  ;i<startingpoint;i++)		//Change the method
		pktbuf_put_char(buf,pkt->data.data[i]);
	
	int client_id=client->client_id ;
	pktbuf_put_char(buf,client_id_map[client_id/(36*36)]);
	client_id %= 36*36 ;
	pktbuf_put_char(buf,client_id_map[client_id/36]);
	client_id %= 36 ; 
	pktbuf_put_char(buf,client_id_map[client_id]);

	for(int i=startingpoint;i<pkt->len;i++)
		pktbuf_put_char(buf,pkt->data.data[i]);					//Change the method

	pktbuf_finish_packet(buf);

	res = pktbuf_send_immediate(buf, client->link);
	pktbuf_reset(buf);
}

/* decide on packets of logged-in client */
static bool handle_client_work(PgSocket *client, PktHdr *pkt)
{
	if(( pkt->type == 'P' || pkt->type == 'B') )
	{	
		/* acquire server */
		if (!find_server(client))
			return false;
	}

	SBuf *sbuf = &client->sbuf;
	int rfq_delta = 0;

	switch (pkt->type) {

	/* one-packet queries */
	case 'Q':		/* Query */
		if (cf_disable_pqexec) {
			slog_error(client, "client used 'Q' packet type");
			disconnect_client(client, true, "PQexec disallowed");
			return false;
		}
		rfq_delta++;
		break;
	case 'F':		/* FunctionCall */
		rfq_delta++;
		break;

	/* request immediate response from server */
	case 'S':		/* Sync */
		rfq_delta++;
		break;
	case 'H':		/* Flush */
		break;

	/* copy end markers */
	case 'c':		/* CopyDone(F/B) */
	case 'f':		/* CopyFail(F/B) */
		break;

	/*
	 * extended protocol allows server (and thus pooler)
	 * to buffer packets until sync or flush is sent by client
	 */
	case 'P':		/* Parse */
		if(pkt->data.data[STMTNAME_START_PARSE] != 0 )
			register_parse_packet(client, pkt);
		break;
	case 'B':		/* Bind */
		create_if_not_exist_ppstmt(client->link,pkt);
	case 'E':		/* Execute */
	case 'C':		/* Close */
	case 'D':		/* Describe */
	case 'd':		/* CopyData(F/B) */
		break;

	/* client wants to go away */
	default:
		slog_error(client, "unknown pkt from client: %u/0x%x", pkt->type, pkt->type);
		disconnect_client(client, true, "unknown pkt");
		return false;
	case 'X': /* Terminate */
		disconnect_client(client, false, "client close request");
		return false;
	}

	/* update stats */
	if (!client->query_start) {
		client->pool->stats.query_count++;
		client->query_start = get_cached_time();
	}

	/* remember timestamp of the first query in a transaction */
	if (!client->xact_start) {
		client->pool->stats.xact_count++;
		client->xact_start = client->query_start;
	}

	if (client->pool->db->admin)
	{
		return admin_handle_client(client, pkt);

	}
		
	/* acquire server */
	if (!find_server(client))
		return false;

	//print_content(client->link, pkt, "client");
	/* postpone rfq change until certain that client will not be paused */
	if (rfq_delta) {
		client->expect_rfq_count += rfq_delta;
	}

	client->pool->stats.client_bytes += pkt->len;

	/* tag the server as dirty */
	client->link->ready = false;
	client->link->idle_tx = false;

	/* forward the packet */

	if(pkt->type == 'B' )
	{	int startppt = starting_point_bind_pkt(pkt);
		if(pkt->data.data[startppt]  != 0)
		{
			send_bind_pkt(client,pkt,startppt);
			sbuf_prepare_skip(sbuf, pkt->len);
			return true ;
		}
	}

	if(pkt->type=='P' && pkt->data.data[STMTNAME_START_PARSE] !=0  )
	{	sbuf_prepare_skip(sbuf, pkt->len);
		return true ; 
	}

	send_immediate_one_pkt(client,pkt);
	sbuf_prepare_skip(sbuf,pkt->len);
	//sbuf_prepare_send(sbuf, &client->link->sbuf, pkt->len);

	return true;
}

/* callback from SBuf */
bool client_proto(SBuf *sbuf, SBufEvent evtype, struct MBuf *data)
{
	bool res = false;
	PgSocket *client = container_of(sbuf, PgSocket, sbuf);
	PktHdr pkt;


	Assert(!is_server_socket(client));
	Assert(client->sbuf.sock);
	Assert(client->state != CL_FREE);

	/* may happen if close failed */
	if (client->state == CL_JUSTFREE)
		return false;

	switch (evtype) {
	case SBUF_EV_CONNECT_OK:
	case SBUF_EV_CONNECT_FAILED:
		/* ^ those should not happen */
	case SBUF_EV_RECV_FAILED:
		/*
		 * Don't log error if client disconnects right away,
		 * could be monitoring probe.
		 */
		if (client->state == CL_LOGIN && mbuf_avail_for_read(data) == 0)
			disconnect_client(client, false, NULL);
		else
			disconnect_client(client, false, "client unexpected eof");
		break;
	case SBUF_EV_SEND_FAILED:
		disconnect_server(client->link, false, "server connection closed");
		break;
	case SBUF_EV_READ:
		/* Wait until full packet headers is available. */
		if (incomplete_header(data)) {
			slog_noise(client, "C: got partial header, trying to wait a bit");
			return false;
		}
		if (!get_header(data, &pkt)) {
			char hex[8*2 + 1];
			disconnect_client(client, true, "bad packet header: '%s'",
					  hdr2hex(data, hex, sizeof(hex)));
			return false;
		}
		slog_noise(client, "read pkt='%c' len=%u", pkt_desc(&pkt), pkt.len);

		/*
		 * If we are reading an SSL request or GSSAPI
		 * encryption request, we should have no data already
		 * buffered at this point.  If we do, it was received
		 * before we performed the SSL or GSSAPI handshake, so
		 * it wasn't encrypted and indeed may have been
		 * injected by a man-in-the-middle.  We report this
		 * case to the client.
		 */
		if (pkt.type == PKT_SSLREQ && mbuf_avail_for_read(data) > 0) {
			disconnect_client(client, true, "received unencrypted data after SSL request");
			return false;
		}
		if (pkt.type == PKT_GSSENCREQ && mbuf_avail_for_read(data) > 0) {
			disconnect_client(client, true, "received unencrypted data after GSSAPI encryption request");
			return false;
		}

		client->request_time = get_cached_time();
		switch (client->state) {
		case CL_LOGIN:
			res = handle_client_startup(client, &pkt);
			break;
		case CL_ACTIVE:
			if (client->wait_for_welcome)
				res = handle_client_startup(client, &pkt);
			else
				res = handle_client_work(client, &pkt);
			break;
		case CL_WAITING:
			fatal("why waiting client in client_proto()");
		default:
			fatal("bad client state: %d", client->state);
		}
		break;
	case SBUF_EV_FLUSH:
		/* client is not interested in it */
		break;
	case SBUF_EV_PKT_CALLBACK:
		/* unused ATM */
		break;
	case SBUF_EV_TLS_READY:
		sbuf_continue(&client->sbuf);
		res = true;
		break;
	}
	return res;
}
