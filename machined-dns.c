#include <event2/dns.h>
#include <event2/dns_struct.h>
#include <event2/util.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-bus.h>
#include <sys/epoll.h>
#include <event2/event.h>

void eventlogcb(int severity, const char *msg){
	const char* fmt;
	switch(severity){
	case EVENT_LOG_DEBUG:
		fmt = SD_DEBUG "%s\n";
		break;
	case EVENT_LOG_MSG:
		fmt = SD_INFO "%s\n";
		break;
	case EVENT_LOG_WARN:
		fmt = SD_WARNING "%s\n";
		break;
	case EVENT_LOG_ERR:
		fmt = SD_ERR "%s\n";
		break;
	default:
		fmt = SD_WARNING "Unknown severity from libevent, message:\n";
		break;
	}
	fprintf(stderr, fmt, msg);
}

struct evdns_server_port ** dns_servers = NULL;
size_t dns_server_count = 0;

int get_ip(struct evdns_server_request* request, const struct evdns_server_question *query, sd_bus* bus){
	int ret = 1;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *message = NULL;
	char type;
	const char *name;
	int r = sd_bus_call_method(bus,
		"org.freedesktop.machine1",
		"/org/freedesktop/machine1",
		"org.freedesktop.machine1.Manager",
		"GetMachineAddresses",
		&error,
		&message,
		"s",
		query->name);

	if(r<0){
		fprintf(stderr, SD_WARNING "Failed to parse machinectl response message: %s %s\n", strerror(-r), query->name);
		ret = -1;
		goto end;
	}

	r = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(iay)");
	if(r<0){
		fprintf(stderr, SD_ERR "Failed to parse response message: %s %s\n", strerror(-r), query->name);
		ret = -1;
		goto end;
	}

	while(sd_bus_message_peek_type(message,&type,&name) > 0){
		r = sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "iay");
		if(r<0){
			fprintf(stderr, SD_ERR "Failed to parse response message: %s\n", strerror(-r));
			ret = -1;
			goto end;
		}

		int inettype;
		r = sd_bus_message_read(message, "i", &inettype);
		if(r<0){
			fprintf(stderr, SD_ERR "Failed to parse response message: %s\n", strerror(-r));
			ret = -1;
			goto end;
		}

		uint8_t *ip;
		size_t ip_size;
		r = sd_bus_message_read_array(message, 'y', (const void**)&ip, &ip_size);
		if(r<0){
			fprintf(stderr, SD_ERR "Failed to parse response message: %s\n", strerror(-r));
			ret = -1;
			goto end;
		}

		if(inettype == 2 && query->type == EVDNS_TYPE_A){
			if(!(ip[0] == 169 && ip[1] == 254)){
				evdns_server_request_add_a_reply(request, query->name, 1, ip, 60);
				ret=0;
			}
		} else if(inettype == 10 && query->type == EVDNS_TYPE_AAAA){
			if(!(ip[0]==0xfe && (ip[1]&0xc0) == 0x80)){
				evdns_server_request_add_aaaa_reply(request, query->name, 1, ip, 60);
				ret=0;
			}
		}

		sd_bus_message_exit_container(message);
	}
	sd_bus_message_exit_container(message);
end:	sd_bus_error_free(&error);
	sd_bus_message_unref(message);
	return ret;
}

void server_callback(struct evdns_server_request *request, void *data){
	int error=DNS_ERR_NONE;
 	for (int i=0; i < request->nquestions; ++i) {
		int ok = -1;
		const struct evdns_server_question *q = request->questions[i];
		ok = get_ip(request, q, data);
		if(ok>0)
			error = DNS_ERR_NOTEXIST;
		if(ok<0 && error==DNS_ERR_NONE)
			error = DNS_ERR_SERVERFAILED;
	}
	evdns_server_request_respond(request, error);
}

int main(){
	if(sd_booted() <= 0){
		fprintf(stderr, SD_EMERG "No systemd or got error\n");
		return 1;
	}
	int fdcount = sd_listen_fds(1);
	if(fdcount < 0){
		fprintf(stderr, SD_EMERG "Error on socket count fetch %s\n", strerror(fdcount));
		return 1;
	}
	event_set_log_callback(eventlogcb);
	struct event_base* evbase = event_base_new();

	sd_bus *bus;
	int r = sd_bus_open_system(&bus);
	if(r < 0){
		fprintf(stderr, SD_EMERG "Failed to connect to system bus: %s\n", strerror(-r));
		return 1;
	}

	for(int i=0;i<fdcount;++i){
		int fd = SD_LISTEN_FDS_START + i;
		if(sd_is_socket(fd, AF_INET, SOCK_DGRAM, -1)){
			evutil_make_socket_nonblocking(fd);
			struct evdns_server_port * dns = evdns_add_server_port_with_base(evbase, fd, 0, server_callback, bus);
			dns_servers = realloc(dns_servers, (dns_server_count++) * sizeof(dns));
			dns_servers[dns_server_count-1] = dns;
		}
	}
	return event_base_dispatch(evbase);
}
