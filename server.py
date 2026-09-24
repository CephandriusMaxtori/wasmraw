from functools import partial
import http.server
import socketserver

PORT = 8000
DIRECTORY = "build-wasm"


class Handler(http.server.SimpleHTTPRequestHandler):

    def __init__(self, *args, **kwargs):
        # Bind the directory parameter to SimpleHTTPRequestHandler
        super().__init__(*args, directory=DIRECTORY, **kwargs)


if __name__ == "__main__":
    with socketserver.TCPServer(("", PORT), Handler) as httpd:
        print(
            f"Serving HTTP on 0.0.0.0 port {PORT} (http://localhost:{PORT}/) from '{DIRECTORY}'..."
        )
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")
