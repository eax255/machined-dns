all: machined-dns

machined-dns: machined-dns.o
	gcc -o $@ $< -lsystemd -levent

clean:
	rm -f machined-dns machined-dns.o 
