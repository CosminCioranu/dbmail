/*
 Copyright (C) 2004 IC & S dbmail@ic-s.nl
 Copyright (C) 2008 NFG Net Facilities Group BV, support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* 
 *
 * implementation for lmtp commands according to RFC 1081 */

#include "dbmail.h"

#define INCOMING_BUFFER_SIZE 512

/* max_errors defines the maximum number of allowed failures */
#define MAX_ERRORS 3

/* max_in_buffer defines the maximum number of bytes that are allowed to be
 * in the incoming buffer */
#define MAX_IN_BUFFER 255

#define THIS_MODULE "lmtp"

extern serverConfig_t *server_conf;

extern volatile sig_atomic_t alarm_occured;

/* allowed lmtp commands */
static const char *const commands[] = {
	"LHLO", "QUIT", "RSET", "DATA", "MAIL",
	"VRFY", "EXPN", "HELP", "NOOP", "RCPT"
};

static int lmtp_tokenizer(ClientSession_t *session, char *buffer);

void send_greeting(ClientSession_t *session)
{
	field_t banner;
	GETCONFIGVALUE("banner", "LMTP", banner);
	if (strlen(banner) > 0)
		ci_write(session->ci, "220 %s %s\r\n", session->hostname, banner);
	else
		ci_write(session->ci, "220 %s LMTP\r\n", session->hostname);
}

static void lmtp_cb_time(void *arg)
{
	ClientSession_t *session = (ClientSession_t *)arg;
	ci_write(session->ci, "221 Connection timeout BYE\r\n");
	session->state = IMAPCS_LOGOUT;
}

static void lmtp_cb_read(void *arg)
{
	char buffer[MAX_LINESIZE];	/* connection buffer */
	ClientSession_t *session = (ClientSession_t *)arg;

	while (ci_readln(session->ci, buffer)) {
		if (lmtp_tokenizer(session, buffer)) {
			lmtp(session);
			client_session_reset_parser(session);
		}
	}
	TRACE(TRACE_DEBUG,"[%p] done", session);
}

static void reset_callbacks(ClientSession_t *session)
{
        session->ci->cb_time = lmtp_cb_time;
        session->ci->cb_read = lmtp_cb_read;

        UNBLOCK(session->ci->rx);
        UNBLOCK(session->ci->tx);

        event_add(session->ci->rev, session->ci->timeout);
        event_add(session->ci->wev, NULL);
}

// socket callbacks.

int lmtp_handle_connection(client_sock *c)
{
	ClientSession_t *session = client_session_new(c);
	client_session_set_timeout(session, server_conf->login_timeout);
	reset_callbacks(session);
        send_greeting(session);
	return 0;
}

int lmtp_error(ClientSession_t * session, const char *formatstring, ...)
{
	va_list argp;
	char *s;

	if (session->error_count >= MAX_ERRORS) {
		ci_write(session->ci, "500 Too many errors, closing connection.\r\n");
		session->SessionResult = 2;	/* possible flood */
		client_session_bailout(session);
		return -3;
	}

	va_start(argp, formatstring);
	s = g_strdup_vprintf(formatstring, argp);
	va_end(argp);
	ci_write(session->ci, s);
	g_free(s);

	session->error_count++;
	return 1;
}

int lmtp_tokenizer(ClientSession_t *session, char *buffer)
{
	char *command = NULL, *value;
	int command_type = 0;

	if (! session->command_type) {
		session->parser_state = FALSE;

		command = buffer;
		strip_crlf(command);

		value = strstr(command, " ");	/* look for the separator */

		if (value) {
			*value++ = '\0';	/* set a \0 on the command end */

			if (strlen(value) == 0)
				value = NULL;	/* no value specified */
		}

		for (command_type = LMTP_STRT; command_type < LMTP_END; command_type++)
			if (strcasecmp(command, commands[command_type]) == 0)
				break;

		/* Invalid command */
		if (command_type == LMTP_END)
			return lmtp_error(session, "500 Invalid command.\r\n");

		/* Commands that are allowed to have no arguments */
		if ((value == NULL) &&
		    !((command_type == LMTP_LHLO) || (command_type == LMTP_DATA) ||
		      (command_type == LMTP_RSET) || (command_type == LMTP_QUIT) ||
		      (command_type == LMTP_NOOP) || (command_type == LMTP_HELP) )) {
			return lmtp_error(session, "500 This command requires an argument.\r\n");
		}
		session->command_type = command_type;

		if (value) session->args = g_list_append(session->args, g_strdup(value));

	}

	if (session->command_type == LMTP_DATA) {
		if (command) {
			if (session->state != IMAPCS_AUTHENTICATED) {
				ci_write(session->ci, "550 Command out of sequence\r\n");
				return TRUE;
			}
			if (g_list_length(session->rcpt) < 1) {
				ci_write(session->ci, "503 No valid recipients\r\n");
				return TRUE;
			}
			if (g_list_length(session->from) < 1) {
				ci_write(session->ci, "554 No valid sender.\r\n");
				return TRUE;
			}
			ci_write(session->ci, "354 Start mail input; end with <CRLF>.<CRLF>\r\n");
			return FALSE;
		}

		if (strncmp(buffer,".\n",2)==0)
			session->parser_state = TRUE;
		else
			g_string_append(session->rbuff, buffer);
	} else
		session->parser_state = TRUE;

	TRACE(TRACE_DEBUG, "[%p] cmd [%d], state [%d] [%s]", session, session->command_type, session->parser_state, buffer);

	return session->parser_state;
}

int lmtp(ClientSession_t * session)
{
	DbmailMessage *msg;
	clientbase_t *ci = session->ci;
	int helpcmd;
	const char *class, *subject, *detail;
	size_t tmplen = 0, tmppos = 0;
	char *tmpaddr = NULL, *tmpbody = NULL, *arg;

	switch (session->command_type) {

	case LMTP_QUIT:
		ci_write(ci, "221 %s BYE\r\n", session->hostname);
		session->state = IMAPCS_LOGOUT;
		return 1;

	case LMTP_NOOP:
		ci_write(ci, "250 OK\r\n");
		return 1;

	case LMTP_RSET:
		ci_write(ci, "250 OK\r\n");
		client_session_reset(session);
		return 1;

	case LMTP_LHLO:
		/* Reply wth our hostname and a list of features.
		 * The RFC requires a couple of SMTP extensions
		 * with a MUST statement, so just hardcode them.
		 * */
		ci_write(ci, "250-%s\r\n250-PIPELINING\r\n"
			"250-ENHANCEDSTATUSCODES\r\n250 SIZE\r\n", 
			session->hostname);
				/* This is a SHOULD implement:
				 * "250-8BITMIME\r\n"
				 * Might as well do these, too:
				 * "250-CHUNKING\r\n"
				 * "250-BINARYMIME\r\n"
				 * */
		client_session_reset(session);
		session->state = IMAPCS_AUTHENTICATED;
		client_session_set_timeout(session, server_conf->timeout);

		return 1;

	case LMTP_HELP:
	
		session->args = g_list_first(session->args);
		if (session->args && session->args->data)
			arg = (char *)session->args->data;
		else
			arg = NULL;

		if (arg == NULL)
			helpcmd = LMTP_END;
		else
			for (helpcmd = LMTP_STRT; helpcmd < LMTP_END; helpcmd++)
				if (strcasecmp (arg, commands[helpcmd]) == 0)
					break;

		TRACE(TRACE_DEBUG, "LMTP_HELP requested for commandtype %d", helpcmd);

		if ((helpcmd == LMTP_LHLO) || (helpcmd == LMTP_DATA) || 
			(helpcmd == LMTP_RSET) || (helpcmd == LMTP_QUIT) || 
			(helpcmd == LMTP_NOOP) || (helpcmd == LMTP_HELP)) {
			ci_write(ci, "%s", LMTP_HELP_TEXT[helpcmd]);
		} else
			ci_write(ci, "%s", LMTP_HELP_TEXT[LMTP_END]);
		return 1;

	case LMTP_VRFY:
		/* RFC 2821 says this SHOULD be implemented...
		 * and the goal is to say if the given address
		 * is a valid delivery address at this server. */
		ci_write(ci, "502 Command not implemented\r\n");
		return 1;

	case LMTP_EXPN:
		/* RFC 2821 says this SHOULD be implemented...
		 * and the goal is to return the membership
		 * of the specified mailing list. */
		ci_write(ci, "502 Command not implemented\r\n");
		return 1;

	case LMTP_MAIL:
		/* We need to LHLO first because the client
		 * needs to know what extensions we support.
		 * */
		if (session->state != IMAPCS_AUTHENTICATED) {
			ci_write(ci, "550 Command out of sequence.\r\n");
			return 1;
		} 
		if (g_list_length(session->from) > 0) {
			ci_write(ci, "500 Sender already received. Use RSET to clear.\r\n");
			return 1;
		}
		/* First look for an email address.
		 * Don't bother verifying or whatever,
		 * just find something between angle brackets!
		 * */

		session->args = g_list_first(session->args);
		if (! (session->args && session->args->data))
			return 1;
		arg = (char *)session->args->data;

		find_bounded(arg, '<', '>', &tmpaddr, &tmplen, &tmppos);

		/* Second look for a BODY keyword.
		 * See if it has an argument, and if we
		 * support that feature. Don't give an OK
		 * if we can't handle it yet, like 8BIT!
		 * */

		/* Find the '=' following the address
		 * then advance one character past it
		 * (but only if there's more string!)
		 * */
		if ((tmpbody = strstr(arg + tmppos, "=")) != NULL)
			if (strlen(tmpbody))
				tmpbody++;

		/* This is all a bit nested now... */
		if (! (tmplen && tmpaddr)) {
			ci_write(ci, "500 No address found.\r\n");
			return 1;
		}
		if (tmpbody) {
			if (MATCH(tmpbody, "8BITMIME")) {   // RFC1652
				ci_write(ci, "500 Please use 7BIT MIME only.\r\n");
				return 1;
			}
			if (MATCH(tmpbody, "BINARYMIME")) { // RFC3030
				ci_write(ci, "500 Please use 7BIT MIME only.\r\n");
				return 1;
			}
		}

		session->from = g_list_prepend(session->from, g_strdup(tmpaddr));
		ci_write(ci, "250 Sender <%s> OK\r\n", (char *)(session->from->data));

		g_free(tmpaddr);

		return 1;

	case LMTP_RCPT:
		if (session->state != IMAPCS_AUTHENTICATED) {
			ci_write(ci, "550 Command out of sequence.\r\n");
			return 1;
		} 

		session->args = g_list_first(session->args);
		if (! (session->args && session->args->data))
			return 1;
		arg = (char *)session->args->data;

		find_bounded(arg, '<', '>', &tmpaddr, &tmplen, &tmppos);

		if (tmplen < 1) {
			ci_write(ci, "500 No address found.\r\n");
			return 1;
		}
		deliver_to_user_t *dsnuser = g_new0(deliver_to_user_t,1);

		dsnuser_init(dsnuser);

		/* find_bounded() allocated tmpaddr for us, and that's ok
		 * since dsnuser_free() will free it for us later on. */
		dsnuser->address = tmpaddr;

		if (dsnuser_resolve(dsnuser) != 0) {
			TRACE(TRACE_ERROR, "dsnuser_resolve_list failed");
			ci_write(ci, "430 Temporary failure in recipient lookup\r\n");
			dsnuser_free(dsnuser);
			return 1;
		}

		/* Class 2 means the address was deliverable in some way. */
		switch (dsnuser->dsn.class) {
			case DSN_CLASS_OK:
				ci_write(ci, "250 Recipient <%s> OK\r\n", dsnuser->address);
				session->rcpt = g_list_prepend(session->rcpt, dsnuser);
				break;
			default:
				ci_write(ci, "550 Recipient <%s> FAIL\r\n", dsnuser->address);
				dsnuser_free(dsnuser);
				break;
		}
		return 1;

	/* Here's where it gets really exciting! */
	case LMTP_DATA:
		msg = dbmail_message_new();
		dbmail_message_init_with_string(msg, session->rbuff);
		dbmail_message_set_header(msg, "Return-Path", (char *)session->from->data);
		g_string_printf(session->rbuff,"%s","");

		if (insert_messages(msg, session->rcpt) == -1) {
			ci_write(ci, "430 Message not received\r\n");
			dbmail_message_free(msg);
			return 1;
		}
		/* The DATA command itself it not given a reply except
		 * that of the status of each of the remaining recipients. */

		/* The replies MUST be in the order received */
		session->rcpt = g_list_reverse(session->rcpt);
		while (session->rcpt) {
			deliver_to_user_t * dsnuser = (deliver_to_user_t *)session->rcpt->data;
			dsn_tostring(dsnuser->dsn, &class, &subject, &detail);

			/* Give a simple OK, otherwise a detailed message. */
			switch (dsnuser->dsn.class) {
				case DSN_CLASS_OK:
					ci_write(ci, "%d%d%d Recipient <%s> OK\r\n",
							dsnuser->dsn.class, dsnuser->dsn.subject, dsnuser->dsn.detail,
							dsnuser->address);
					break;
				default:
					ci_write(ci, "%d%d%d Recipient <%s> %s %s %s\r\n",
							dsnuser->dsn.class, dsnuser->dsn.subject, dsnuser->dsn.detail,
							dsnuser->address, class, subject, detail);
			}
			if (! g_list_next(session->rcpt)) break;
			session->rcpt = g_list_next(session->rcpt);
		}
		dbmail_message_free(msg);
		return 1;

	default:
		return lmtp_error(session, "500 What are you trying to say here?\r\n");

	}
	return 1;
}

