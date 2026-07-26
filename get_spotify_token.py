"""
One-time script to get a Spotify refresh token.
Run this once on your computer, copy the refresh token it prints,
you'll paste that into the ESP32 sketch later.
"""

import requests
import base64
import webbrowser
import http.server
import urllib.parse

# ==== PASTE YOUR CREDENTIALS HERE ====
CLIENT_ID = "3a709de97bdd4878ae7fc4274a1abb6f"
CLIENT_SECRET = "5e4bc139ff934c768f03e37d22ba31c8"
# ======================================

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPE = "user-read-currently-playing user-read-playback-state"

auth_code = None

class CallbackHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        global auth_code
        query = urllib.parse.urlparse(self.path).query
        params = urllib.parse.parse_qs(query)
        if "code" in params:
            auth_code = params["code"][0]
            self.send_response(200)
            self.send_header("Content-type", "text/html")
            self.end_headers()
            self.wfile.write(b"<h1>Success! You can close this tab and go back to the terminal.</h1>")
        else:
            self.send_response(400)
            self.end_headers()

    def log_message(self, format, *args):
        pass  # silence default logging

def main():
    auth_url = (
        "https://accounts.spotify.com/authorize"
        f"?client_id={CLIENT_ID}"
        "&response_type=code"
        f"&redirect_uri={urllib.parse.quote(REDIRECT_URI)}"
        f"&scope={urllib.parse.quote(SCOPE)}"
    )

    print("opening browser for spotify login...")
    webbrowser.open(auth_url)

    server = http.server.HTTPServer(("127.0.0.1", 8888), CallbackHandler)
    print("waiting for you to approve the app in your browser...")
    server.handle_request()  # handles exactly one request then stops

    if not auth_code:
        print("didn't get an auth code, something went wrong")
        return

    print("got auth code, exchanging for tokens...")

    auth_header = base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode()).decode()

    response = requests.post(
        "https://accounts.spotify.com/api/token",
        headers={
            "Authorization": f"Basic {auth_header}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        data={
            "grant_type": "authorization_code",
            "code": auth_code,
            "redirect_uri": REDIRECT_URI,
        },
    )

    tokens = response.json()

    if "refresh_token" in tokens:
        print("\n" + "=" * 50)
        print("SUCCESS! here's your refresh token:")
        print(tokens["refresh_token"])
        print("=" * 50)
        print("\nsave this somewhere, you'll need it for the ESP32 sketch")
    else:
        print("something went wrong, response was:")
        print(tokens)

if __name__ == "__main__":
    main()
