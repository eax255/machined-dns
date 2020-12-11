all: machined-dns

machined-dns: machined-dns.o
	gcc -o $@ $< -lsystemd -levent

install: machined-dns
	install -s machined-dns /usr/local/bin/
	install machined-dns.socket /etc/systemd/system/
	install machined-dns.service /etc/systemd/system/

clean:
	rm -f machined-dns machined-dns.o 
