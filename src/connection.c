#include "connection.h"

void return_upstream_connection(uv_tcp_t* stream,
                                upstream_connection **pool)
{
	upstream_connection *new = malloc(sizeof(*new));

	// Add to the front of the list
	new->previous = *pool;
	new->stream = stream;
	*pool = new;
}


uv_tcp_t* upstream_from_pool(upstream_connection **pool)
{
	uv_tcp_t *ret;

	if(!(*pool))
		return NULL;
	else
	{
		ret = (*pool)->stream;
		*pool = (*pool)->previous;
		return ret;
	}
}


void free_conn_pool(upstream_connection *pool)
{
	upstream_connection *conn;
	while(pool)
	{
		conn = pool->previous;
		uv_close((uv_handle_t*)pool->stream, free_handle);
		free(pool);
		pool = conn;
	}
}


void free_handle(uv_handle_t *handle)
{
	free(handle);
}

void upstream_disconnected(upstream_connection **pool, uv_tcp_t* connection)
{
	upstream_connection *previous = NULL;
	upstream_connection *i = *pool;

	while(i)
	{
		if(i->stream == connection)
		{
			if(previous)
			{
				// If we're removing a list element not at the top, remove
				//  this element and set the one before to point where this one
				//  used to
				previous->previous = i->previous;
				free(i);
			}
			else
			{
				// If this is at the top of the queue we just pop it off moving
				//  the next one up to the top
				*pool = i->previous;
				free(i);
			}
			return;
		}
		previous = i;
		i = i->previous;
	}
}


int resolve_address(uv_loop_t *loop, char *address, resolve_callback *callback)
{
	int nodelen, servicelen;
	char *node_start, *node_end, *node, *service_start, *service;
	uv_getaddrinfo_t *lookup_req;

	printf("%s\n", address);
	if(!strncasecmp("tcp://", address, 6))
	{
		node_start = address + 6;
		node_end = strchr(node_start, ':');
		if(!node_end)
			return 0;
		nodelen = node_end - node_start;
		node = malloc(nodelen + 1);
		memcpy(node, node_start, nodelen);
		node[nodelen] = '\0';

		service_start = node_end + 1;
		servicelen = strlen(service_start);
		service = malloc(servicelen + 1);
		memcpy(service, service_start, servicelen);
		service[servicelen] = '\0';

		lookup_req = malloc(sizeof(*lookup_req));
		lookup_req->data = callback;
		uv_getaddrinfo(loop, lookup_req, resolve_address_cb, node, service,
		               NULL);
	}
	else if(!strncasecmp("unix://", address, 7))
	{
		// TODO actual unix socket support
		printf("unix");
	}
	else
	{
		log_message(LOG_ERROR, "Unknown socket type for address %s\n",
		            address);
		return 0;
	}
	return 1;
}


void resolve_address_cb(uv_getaddrinfo_t *req, int status,
                        struct addrinfo *res)
{
	resolve_callback *callback = req->data;

	if(status < 0)
	{
		log_message(LOG_ERROR, "Address resolution error: %s\n",
		            uv_err_name(status));
		return;
	}

	callback->callback(req, res);
}


void bind_on_and_listen(uv_getaddrinfo_t *req, struct addrinfo *res)
{
	int ret;
	resolve_callback *callback = req->data;
	bind_and_listen_data *data = callback->data;

	// Initialize listening tcp socket
	ret = uv_tcp_init(req->loop, data->listener);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed to initialize tcp socket: %s\n",
		            uv_err_name(ret));
		return;
	}

	// Bind address
	ret = uv_tcp_bind(data->listener, res->ai_addr, 0);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed to bind: %s\n", uv_err_name(ret));
		return;
	}

	// Begin listening
	data->listener->data = data->accept_cb;
	ret = uv_listen((uv_stream_t*)data->listener, data->backlog_size,
	                proxy_accept_client);
	if(ret)
	{
		log_message(LOG_ERROR, "Listen error: %s\n", uv_err_name(ret));
		return;
	}
}


/*int init_proxy_listen_socket(char *address)
{
	int ret;
	struct sockaddr_in bind_addr;
	tcp_proxy_config *cfg = config;
	accept_callback *callback;

	log_message(LOG_INFO, "tcp proxy starting!\n");

	// Resolve listen address
	ret = uv_ip4_addr(cfg->listen_host, cfg->listen_port, &bind_addr);
	if(ret)
	{
		log_message(LOG_ERROR, "Listen socket resolution error: %s\n",
		            uv_err_name(ret));
		return MOD_ERROR;
	}

	// Resolve upstream address
	cfg->upstream_addr = malloc(sizeof(*cfg->upstream_addr));
	ret = uv_ip4_addr(cfg->upstream_host, cfg->upstream_port,
	                  cfg->upstream_addr);
	if(ret)
	{
		log_message(LOG_ERROR, "Upstream socket resolution error: %s\n",
		            uv_err_name(ret));
		return MOD_ERROR;
	}

	// Initialize listening tcp socket
	ret = uv_tcp_init(master_loop, cfg->listener);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed to initialize tcp socket: %s\n",
		            uv_err_name(ret));
		return MOD_ERROR;
	}

	// Bind address
	ret = uv_tcp_bind(cfg->listener, (struct sockaddr*)&bind_addr, 0);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed to bind on %s:%d: %s\n",
		            cfg->listen_host, cfg->listen_port,uv_err_name(ret));
		return MOD_ERROR;
	}

	// Allocate new client callback
	callback = malloc(sizeof(*callback));
	callback->callback = tcp_proxy_new_client;
	callback->data = cfg;
	cfg->accept_cb = callback;
	cfg->listener->data = callback;

	// Begin listening
	ret = uv_listen((uv_stream_t*)cfg->listener, cfg->backlog_size,
	                proxy_accept_client);
	if(ret)
	{
		log_message(LOG_ERROR, "Listen error: %s\n", uv_err_name(ret));
		return MOD_ERROR;
	}

	return MOD_OK;
}*/


void proxy_accept_client(uv_stream_t *listener, int status)
{
	int ret;
	proxy_client *new;
	accept_callback *callback;

	log_message(LOG_INFO, "New client connection\n");

	if(status)
	{
		log_message(LOG_ERROR, "Error accepting new client\n");
		return;
	}

	// Allocate our client structure
	new = malloc(sizeof(*new));

	// Set reference to listener stream and data pointer
	new->server = listener;
	callback = listener->data;
	new->data = callback->data;

	// Initialize downstream connection
	new->downstream = malloc(sizeof(*new->downstream));
	ret = uv_tcp_init(listener->loop, new->downstream);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed initializing new client socket\n");
		return;
	}

	// Accept connection
	new->downstream->data = new;
	ret = uv_accept(new->server, (uv_stream_t*)new->downstream);
	if(ret)
	{
		log_message(LOG_ERROR, "Failed to accept new client socket\n");
		return;
	}

	// Run callback
	callback->callback(new, listener);
}
