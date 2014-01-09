#!/usr/bin/env python
#coding: utf-8
import socket

def recv_conn(addr):
	s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	s.bind(addr)
	s.listen(5)
	while True:
		client = s.accept()
		yield client

def main():
	for c, a in recv_conn(("", 9000)):
		c.send("Hello World\n")
		c.close()

if __name__ == "__main__":
	main()
