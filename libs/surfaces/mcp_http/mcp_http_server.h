/*
 * Copyright (C) 2026 Frank Povazanj <frank.povazanj@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _ardour_surface_mcp_http_server_h_
#define _ardour_surface_mcp_http_server_h_

#include <atomic>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <stdint.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libwebsockets.h>

#include "pbd/signals.h"

namespace PBD
{
class EventLoop;
}

namespace ARDOUR
{
class Session;
}

namespace ArdourSurface
{

class MCPHttpServer
{
public:
	MCPHttpServer (ARDOUR::Session&, uint16_t port, int debug_level, PBD::EventLoop* event_loop);
	~MCPHttpServer ();

	int  start ();
	int  stop ();
	void set_debug_level (int);
	int  debug_level () const;

private:
	struct ClientContext {
		bool        mcp_post;
		bool        have_response;
		std::string request_body;
		std::string response_body;
		/* SSE-specific fields */
		bool                    sse_client = false;
		std::deque<std::string> sse_queue;
		std::mutex              sse_queue_mutex;
	};

	struct SseSubscriber {
		struct lws* wsi;
	};
	typedef std::vector<SseSubscriber*> SseSubscriberList;

	typedef std::unordered_map<struct lws*, ClientContext> ClientMap;

	ARDOUR::Session&                 _session;
	uint16_t                         _port;
	std::atomic<int>                 _debug_level;
	PBD::EventLoop*                  _event_loop;
	struct lws_context*              _context;
	struct lws_protocols             _protocols[2];
	struct lws_context_creation_info _info;
	ClientMap                        _clients;
	std::thread                      _service_thread;
	bool                             _running;

	/* SSE subscriber registry — protected by _sse_subscribers_mutex.
	 * Entries are inserted on LWS_CALLBACK_HTTP (/events) and removed on
	 * LWS_CALLBACK_CLOSED_HTTP, both of which run on the lws service thread.
	 * broadcast_sse() may be called from the GUI event-loop thread and
	 * only reads the list under the mutex before calling lws_cancel_service. */
	SseSubscriberList                _sse_subscribers;
	std::mutex                       _sse_subscribers_mutex;
	PBD::ScopedConnectionList        _sse_signal_connections;
	time_t                           _sse_last_heartbeat;

	void run ();

	ClientContext& client (struct lws*);
	void           erase_client (struct lws*);

	int callback (struct lws*, enum lws_callback_reasons, void*, void*, size_t);
	int handle_http (struct lws*, ClientContext&);
	int handle_http_body (struct lws*, ClientContext&, void*, size_t);
	int handle_http_body_completion (struct lws*, ClientContext&);
	int handle_http_writeable (struct lws*, ClientContext&);

	int send_json_headers (struct lws*);
	int send_http_status (struct lws*, unsigned int);
	int write_json_response (struct lws*, ClientContext&);

	/* SSE helpers */
	int         send_sse_headers (struct lws*);
	void        broadcast_sse (const std::string& sse_frame);
	void        on_transport_state_changed ();
	std::string build_transport_event () const;
	void        connect_transport_signals ();

	std::string dispatch_jsonrpc (const std::string&) const;

	static int lws_callback (struct lws*, enum lws_callback_reasons, void*, void*, size_t);
};

} // namespace ArdourSurface

#endif // _ardour_surface_mcp_http_server_h_
