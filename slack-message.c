#include <debug.h>
#include <version.h>

#include "slack-json.h"
#include "slack-rtm.h"
#include "slack-api.h"
#include "slack-user.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-message.h"

gchar *slack_html_to_message(SlackAccount *sa, const char *s, PurpleMessageFlags flags) {

	if (flags & PURPLE_MESSAGE_RAW)
		return g_strdup(s);

	GString *msg = g_string_sized_new(strlen(s));
	while (*s) {
		const char *ent;
		int len;
		if ((*s == '@' || *s == '#') && !(flags & PURPLE_MESSAGE_NO_LINKIFY)) {
			const char *e = s+1;
			/* try to find the end of this command, but not very well -- not sure what characters are valid and eventually will need to deal with spaces */
			while (g_ascii_isalnum(*e) || *e == '-' || *e == '_' || (*e == '.' && g_ascii_isalnum(e[1]))) e++;
			if (*s == '@') {
#define COMMAND(CMD, CMDL) \
				if (e-(s+1) == CMDL && !strncmp(s+1, CMD, CMDL)) { \
					g_string_append_len(msg, "<!" CMD ">", CMDL+3); \
					s = e; \
					continue; \
				}
				COMMAND("here", 4)
				COMMAND("channel", 7)
				COMMAND("everyone", 8)
			}
#undef COMMAND
			char *t = g_strndup(s+1, e-(s+1));
			SlackObject *obj = g_hash_table_lookup(*s == '@' ? sa->user_names : sa->channel_names, t);
			g_free(t);
			if (obj) {
				g_string_append_c(msg, '<');
				g_string_append_c(msg, *s);
				g_string_append(msg, obj->id);
				g_string_append_c(msg, '|');
				g_string_append_len(msg, s+1, e-(s+1));
				g_string_append_c(msg, '>');
				s = e;
				continue;
			}
		}
		if ((ent = purple_markup_unescape_entity(s, &len))) {
			if (!strcmp(ent, "&"))
				g_string_append(msg, "&amp;");
			else if (!strcmp(ent, "<"))
				g_string_append(msg, "&lt;");
			else if (!strcmp(ent, ">"))
				g_string_append(msg, "&gt;");
			else
				g_string_append(msg, ent);
			s += len;
			continue;
		}
		if (!g_ascii_strncasecmp(s, "<br>", 4)) {
			g_string_append_c(msg, '\n');
			s += 4;
			continue;
		}
		/* what about other tags? urls (auto-detected server-side)? dates? */
		g_string_append_c(msg, *s++);
	}

	return g_string_free(msg, FALSE);
}

void slack_message_to_html(GString *html, SlackAccount *sa, gchar *s, PurpleMessageFlags *flags, gchar *prepend_newline_str) {
	if (!s)
		return;

	if (flags)
		*flags |= PURPLE_MESSAGE_NO_LINKIFY;

	size_t l = strlen(s);
	char *end = &s[l];

	while (s < end) {
		char c = *s++;
		if (c == '\n') {
			g_string_append(html, "<BR>");
			
			// This is here for attachments.  If this message is part of an attachment,
			// we must add the preprend string after every newline.
			if (prepend_newline_str) {
				g_string_append(html, prepend_newline_str);
			}
			continue;
		}
		if (c != '<') {
			g_string_append_c(html, c);
			continue;
		}

		/* found a <tag> */
		char *r = memchr(s, '>', end-s);
		if (!r)
			/* should really be error */
			r = end;
		else
			*r = 0;
		char *b = memchr(s, '|', r-s);
		if (b) {
			*b = 0;
			b++;
		}
		switch (*s) {
			case '#':
				s++;
				g_string_append_c(html, '#');
				if (!b) {
					SlackChannel *chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, s);
					if (chan)
						b = chan->object.name;
				}
				g_string_append(html, b ?: s);
				break;
			case '@':
				s++;
				g_string_append_c(html, '@');
				SlackUser *user = NULL;
				if (slack_object_id_is(sa->self->object.id, s)) {
					user = sa->self;
					if (flags)
						*flags |= PURPLE_MESSAGE_NICK;
				}
				if (!b) {
					if (!user)
						user = (SlackUser*)slack_object_hash_table_lookup(sa->users, s);
					if (user)
						b = user->object.name;
				}
				g_string_append(html, b ?: s);
				break;
			case '!':
				s++;
				if (!strcmp(s, "channel") || !strcmp(s, "group") || !strcmp(s, "here") || !strcmp(s, "everyone")) {
					if (flags)
						*flags |= PURPLE_MESSAGE_NICK;
					g_string_append_c(html, '@');
					g_string_append(html, b ?: s);
				} else {
					g_string_append(html, "&lt;");
					g_string_append(html, b ?: s);
					g_string_append(html, "&gt;");
				}
				break;
			default:
				/* URL */
				g_string_append(html, "<A HREF=\"");
				g_string_append(html, s); /* XXX embedded quotes? */
				g_string_append(html, "\">");
				g_string_append(html, b ?: s);
				g_string_append(html, "</A>");
		}
		s = r+1;
	}
}

/*
 *	Changes a "slack color" (i.e. "good", "warning", "danger") to the correct
 *  RGB color.  From https://api.slack.com/docs/message-attachments
 */
static const gchar *get_color(const char *c) {
	if (c == NULL) {
		return (gchar * ) "#717274";
	} else if (!strcmp(c, "good")) {
		return (gchar * ) "#2fa44f";
	} else if (!strcmp(c, "warning")) {
		return (gchar * ) "#de9e31";
	} else if (!strcmp(c, "danger")) {
		return (gchar * ) "#d50200";
	} else {
		return (gchar * ) c;
	}
}

/*
 * make a link if url is not NULL.  Otherwise, just give the text back.
 */
static void link_html(GString *html, char *url, char *text) {
	if (!text) {
		return;
	} else if (url) {
		g_string_append_printf(
			html,
			"<a href=\"%s\">%s</a>",
			url,
			text
		);
	} else {
		g_string_append(html, text);
	}
}

/*
 * Converts a single attachment to HTML.  The shape of an attachment is
 * documented at https://api.slack.com/docs/message-attachments
 */
static void slack_attachment_to_html(GString *html, SlackAccount *sa, json_value *attachment) {
	char *service_name = json_get_prop_strptr(attachment, "service_name");
	char *service_link = json_get_prop_strptr(attachment, "service_link");
	char *author_name = json_get_prop_strptr(attachment, "author_name");
	char *author_subname = json_get_prop_strptr(attachment, "author_subname");
	
	char *author_link = json_get_prop_strptr(attachment, "author_link");
	char *text = json_get_prop_strptr(attachment, "text");

	//char *fallback = json_get_prop_strptr(attachment, "fallback");
	char *pretext = json_get_prop_strptr(attachment, "pretext");
	
	char *title = json_get_prop_strptr(attachment, "title");
	char *title_link = json_get_prop_strptr(attachment, "title_link");
	char *footer = json_get_prop_strptr(attachment, "footer");
	GString *attachment_prefix = g_string_new(NULL);

	g_string_printf(attachment_prefix,
		"<font color=\"%s\">%s</font>",
		get_color(json_get_prop_strptr(attachment, "color")),
		purple_account_get_string(sa->account, "attachment_prefix", "▎ ")
	);

	GString *brtag = g_string_new("<br/>");
	g_string_append(brtag,
		attachment_prefix->str
	);

	time_t ts = slack_parse_time(json_get_prop(attachment, "ts"));


	// Sometimes, the text of the attachment can be *really* large.  The official
	// Slack client will truncate the text at x-characters and have a "Read More"
	// link so the user can read the rest of the text.  I wasn't sure what the
	// right thing to do for the plugin, so I implemented both the truncated
	// version as well as the "just dump all the text naively" version (the
	// latter of which is what is uncommented .. I am leaving the truncated
	// version commented in in case it is decided that this is the right thing to
	// do.

	/* GString *truncated_text = g_string_sized_new(0);
	g_string_printf(
		truncated_text,
		"%.480s%s",
		(char *) formatted_text,
		(strlen(formatted_text) > 480) ? "…" : ""
	); */

	// pretext
	if (pretext) {
		g_string_append(html, brtag->str);
		slack_message_to_html(html, sa, pretext, NULL, attachment_prefix->str);
	}

	// service name and author name
	if (service_name != NULL || author_name != NULL || author_subname != NULL) {
		g_string_append(html, brtag->str);
		g_string_append(html, "<b>");
		link_html(html, service_link, service_name);
		if (service_name && author_name)
			g_string_append(html, " - ");
		link_html(html, author_link, author_name);
		if (author_subname)
			g_string_append(html, author_subname);
		g_string_append(html, "</b>");
	}

	// title
	if (title) {
		g_string_append(html, brtag->str);
		g_string_append(html, "<b><i>");
		link_html(html, title_link, title);
		g_string_append(html, "</i></b>");
	}

	// main text
	if (text) {
		g_string_append(html, brtag->str);
		g_string_append(html, "<i>");
		slack_message_to_html(html, sa, text, NULL, attachment_prefix->str);
		g_string_append(html, "</i>");
	}

	// fields
	json_value *fields = json_get_prop_type(attachment, "fields", array);
	if (fields) {
		for (int i=0; i<fields->u.array.length; i++) {
			json_value *field = fields->u.array.values[i];
			char *title = json_get_prop_strptr(field, "title");
			char *value = json_get_prop_strptr(field, "value");

			g_string_append_printf(html, "<br />%s<b>", attachment_prefix->str);

			// Run the title through the conversion to html.
			slack_message_to_html(html, sa, title, NULL, attachment_prefix->str);
			g_string_append(html, "</b>: <i>");

			// Run the value through the conversion to html.
			slack_message_to_html(html, sa, value, NULL, attachment_prefix->str);
			g_string_append(html, "</i>");
		}
	}

	// footer
	if (footer) {
		g_string_append(html, brtag->str);
		g_string_append(html, footer);
	}
	if (ts) {
		g_string_append(html, brtag->str);
		g_string_append(html, ctime(&ts));
	}

	g_string_free(brtag, TRUE);
	g_string_free(attachment_prefix, TRUE);
}

static void slack_file_to_html(GString *html, SlackAccount *sa, json_value *file) {
	char *title = json_get_prop_strptr(file, "title");
	char *url = json_get_prop_strptr(file, "url_private");
	if (!url)
		url = json_get_prop_strptr(file, "permalink");

	g_string_append_printf(html, "<br/>%s<a href=\"%s\">%s</a>",
		purple_account_get_string(sa->account, "attachment_prefix", "▎ "),
		url ?: "",
		title ?: "file");
}

void slack_json_to_html(GString *html, SlackAccount *sa, json_value *message, PurpleMessageFlags *flags) {
	const char *subtype = json_get_prop_strptr(message, "subtype");
	int i;
	
	if (flags && json_get_prop_boolean(message, "hidden", FALSE))
		*flags |= PURPLE_MESSAGE_INVISIBLE;

	if (!g_strcmp0(subtype, "me_message"))
		g_string_append(html, "/me ");
	else if (subtype && flags)
		*flags |= PURPLE_MESSAGE_SYSTEM;

	json_value *thread = json_get_prop(message, "thread_ts");
	if (thread) {
		time_t tt = slack_parse_time(thread);
		g_string_append(html, purple_time_format(localtime(&tt)));
		g_string_append(html, "⤷  ");
	}

	slack_message_to_html(html, sa, json_get_prop_strptr(message, "text"), flags, NULL);

	json_value *files = json_get_prop_type(message, "files", array);
	if (files)
		for (i=0; i < files->u.array.length; i++)
			slack_file_to_html(html, sa, files->u.array.values[i]);

	// If there are attachements, show them.
	json_value *attachments = json_get_prop_type(message, "attachments", array);
	if (attachments)
		for (i=0; i < attachments->u.array.length; i++)
			slack_attachment_to_html(html, sa, attachments->u.array.values[i]);
}

void slack_handle_message(SlackAccount *sa, SlackObject *obj, json_value *json, PurpleMessageFlags flags) {
	if (!obj) {
		purple_debug_warning("slack", "Message to unknown channel %s\n", json_get_prop_strptr(json, "channel"));
		return;
	}
	const char *subtype     = json_get_prop_strptr(json, "subtype");
	json_value *ts          = json_get_prop(json, "ts");
	time_t mt = slack_parse_time(ts);
	json_value *message     = json;
	GString *html = g_string_new(NULL);

	if (!g_strcmp0(subtype, "message_changed")) {
		message = json_get_prop(json, "message");
		json_value *old_message = json_get_prop(json, "previous_message");
		/* this may consist only of added attachments, no changed text */
		gboolean changed = g_strcmp0(json_get_prop_strptr(message, "text"), json_get_prop_strptr(old_message, "text"));
		g_string_append(html, "<font color=\"#717274\"><i>[edit]</i></font> ");
		slack_json_to_html(html, sa, message, &flags);
		if (old_message && changed) {
			g_string_append(html, "<br>(Old message: ");
			slack_json_to_html(html, sa, old_message, NULL);
			g_string_append(html, ")");
		}
	}
	else if (!g_strcmp0(subtype, "message_deleted")) {
		message = json_get_prop(json, "previous_message");
		g_string_append(html, "(<font color=\"#717274\"><i>Deleted message</i></font>");
		if (message) {
			g_string_append(html, ": ");
			slack_json_to_html(html, sa, message, &flags);
		}
		g_string_append(html, ")");
	}
	else
		slack_json_to_html(html, sa, message, &flags);

	if (!html->len) {
		/* if after all of that we still have no message, just dump it */
		g_string_free(html, TRUE);
		purple_debug_info("slack", "Ignoring unparsed message\n");
		return;
	}

	const char *user_id = json_get_prop_strptr(message, "user");
	SlackUser *user = NULL;
	if (slack_object_id_is(sa->self->object.id, user_id)) {
		user = sa->self;
#if PURPLE_VERSION_CHECK(2,12,0)
		flags |= PURPLE_MESSAGE_REMOTE_SEND;
#else
		flags |= 0x10000;
#endif
		flags |= PURPLE_MESSAGE_SEND;
		flags &= ~PURPLE_MESSAGE_RECV;
	}
	/* for bots providing different display name */
	const char *username = json_get_prop_strptr(message, "username");
	if (username)
		flags &= ~PURPLE_MESSAGE_SYSTEM;

	PurpleConversation *conv = NULL;
	if (SLACK_IS_CHANNEL(obj)) {
		SlackChannel *chan = (SlackChannel*)obj;
		/* Channel */
		if (!chan->cid) {
			if (!purple_account_get_bool(sa->account, "open_chat", FALSE)) {
				g_string_free(html, TRUE);
				return;
			}
			slack_chat_open(sa, chan);
		}

		if (!user)
			user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);

		PurpleConvChat *chat = slack_channel_get_conversation(sa, chan);
		if (chat) {
			conv = purple_conv_chat_get_conversation(chat);
			if (!subtype);
			else if (!strcmp(subtype, "channel_topic") ||
					!strcmp(subtype, "group_topic"))
				purple_conv_chat_set_topic(chat, user ? user->object.name : user_id, json_get_prop_strptr(json, "topic"));
		}
		
		serv_got_chat_in(sa->gc, chan->cid, user ? user->object.name : user_id ?: username ?: "", flags, html->str, mt);
	} else if (SLACK_IS_USER(obj)) {
		SlackUser *im = (SlackUser*)obj;
		/* IM */
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, im->object.name, sa->account);
		if (slack_object_id_is(im->object.id, user_id))
			serv_got_im(sa->gc, im->object.name, html->str, flags, mt);
		else {
			if (!conv)
				conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, sa->account, im->object.name);
			if (!user)
				/* is this necessary? shouldn't be anyone else in here */
				user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
			purple_conversation_write(conv, user ? user->object.name : user_id ?: username, html->str, flags, mt);
		}
	}

	g_string_free(html, TRUE);

	/* update most recent ts for later marking */
	const char *tss = json_get_strptr(ts);
	if (slack_ts_cmp(tss, obj->last_mesg) > 0) {
		g_free(obj->last_mesg);
		obj->last_mesg = g_strdup(tss);
	}
}

static void handle_message(SlackAccount *sa, gpointer data, SlackObject *obj) {
	json_value *json = data;
	return slack_handle_message(sa, obj, json, PURPLE_MESSAGE_RECV);
	json_value_free(json);
}

gboolean slack_message(SlackAccount *sa, json_value *json) {
	slack_conversation_retrieve(sa, json_get_prop_strptr(json, "channel"), handle_message, json);
	return TRUE;
}

void slack_user_typing(SlackAccount *sa, json_value *json) {
	const char *user_id    = json_get_prop_strptr(json, "user");
	const char *channel_id = json_get_prop_strptr(json, "channel");

	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
	SlackChannel *chan;
	if (user && slack_object_id_is(user->im, channel_id)) {
		/* IM */
		serv_got_typing(sa->gc, user->object.name, 4, PURPLE_TYPING);
	} else if (user && (chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, channel_id))) {
		/* Channel */
#if 0
		PurpleConvChat *chat = slack_channel_get_conversation(sa, chan);
		PurpleConvChatBuddy *cb = chat ? purple_conv_chat_cb_find(chat, user->object.name) : NULL;
		if (cb) {
			purple_conv_chat_user_set_flags(chat, user->object.name, cb->flags | PURPLE_CBFLAGS_TYPING);
			/* TODO: clear typing after 4 seconds (like purple_conv_im typing_timeout), but only if no more recent update, and don't crash if we're gone:
			 * schedule a timeout and set a cb->attribute with the current time? keep a list of current typing chat/user pairs? */
		}
#endif
	} else {
		purple_debug_warning("slack", "Unhandled typing: %s@%s\n", user_id, channel_id);
	}
}

unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state) {
	SlackAccount *sa = gc->proto_data;

	if (state != PURPLE_TYPING)
		return 0;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user || !*user->im)
		return 0;

	GString *channel = append_json_string(g_string_new(NULL), user->im);
	slack_rtm_send(sa, NULL, NULL, "typing", "channel", channel->str, NULL);
	g_string_free(channel, TRUE);

	return 3;
}
