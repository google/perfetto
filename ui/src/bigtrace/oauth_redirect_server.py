import os
import secrets
import logging
import json
from datetime import datetime, timezone, timedelta
from functools import wraps
from urllib.parse import urlencode

import httpx
from flask import Flask, request, redirect, jsonify, make_response, Blueprint
from google.cloud import secretmanager
from google.api_core import exceptions
import google.auth
import jwt
from cryptography.fernet import Fernet, InvalidToken

# --- Logging Setup ---
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# --- Core Application Setup ---
app = Flask(__name__)

# --- Classes for Organization ---

class Config:
    """Application configuration variables."""
    REDIRECT_URI = "https://brush-corprun-dev.gclb.goog/auth/callback"
    GOOGLE_AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
    GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token"
    GOOGLE_USERINFO_URL = "https://www.googleapis.com/oauth2/v2/userinfo"
    SCOPE = "openid email profile"

class SecretManager:
    """Handles fetching secrets from GCP, with smart caching."""
    _cache = {}

    @classmethod
    def get_secret(cls, secret_name: str) -> str:
        """Fetch a secret, caching it to avoid repeated lookups."""
        if secret_name in cls._cache:
            return cls._cache[secret_name]

        _, project_id = google.auth.default()
        client = secretmanager.SecretManagerServiceClient()
        name = f"projects/{project_id}/secrets/{secret_name}/versions/latest"

        try:
            response = client.access_secret_version(request={"name": name})
            secret_value = response.payload.data.decode("UTF-8")
            cls._cache[secret_name] = secret_value
            return secret_value
        except exceptions.NotFound:
            logger.error(f"Critical: Secret '{secret_name}' not found in GCP Secret Manager.")
            raise
        except Exception as e:
            logger.error(f"Failed to retrieve secret '{secret_name}': {e}")
            raise

    @classmethod
    def clear_cache(cls, secret_name: str = None):
        """Clears the entire cache or a specific secret."""
        if secret_name:
            if secret_name in cls._cache:
                del cls._cache[secret_name]
                logger.info(f"Cache cleared for secret: {secret_name}")
        else:
            cls._cache.clear()
            logger.info("Cleared all cached secrets.")

def _google_api_request_with_retry(method, url, attempts=2, **kwargs):
    """
    Makes a request to a Google API, transparently handling secret rotation
    by retrying once on a 401 Unauthorized error.
    """
    last_exception = None
    for attempt in range(attempts):
        try:
            # On a retry, the secrets will be re-fetched because the cache was cleared.
            if 'data' in kwargs:
                 kwargs['data']['client_id'] = SecretManager.get_secret("oauth-client-id")
                 kwargs['data']['client_secret'] = SecretManager.get_secret("oauth-client-secret")

            response = httpx.request(method, url, **kwargs)
            response.raise_for_status()
            return response
        except httpx.HTTPStatusError as e:
            last_exception = e
            if e.response.status_code == 401 and (attempt + 1) < attempts:
                logger.warning(
                    f"Google API request failed with 401. Clearing secrets and retrying... "
                    f"(Attempt {attempt + 1}/{attempts})"
                )
                SecretManager.clear_cache("oauth-client-id")
                SecretManager.clear_cache("oauth-client-secret")
                continue # Retry the loop
            else:
                # Re-raise for non-401 errors or if all retries are exhausted
                raise
    # This line should not be reached if the loop runs at least once.
    raise last_exception


class SecureSession:
    """Handles creation and verification of secure, encrypted session cookies."""
    _fernet = None

    @classmethod
    def _get_fernet(cls):
        if not cls._fernet:
            encryption_key = SecretManager.get_secret("session-encryption-key")
            cls._fernet = Fernet(encryption_key.encode())
        return cls._fernet

    @staticmethod
    def create(data: dict, hours: int = 24) -> str:
        payload = {
            **data,
            "exp": (datetime.now(timezone.utc) + timedelta(hours=hours)).isoformat(),
            "iat": datetime.now(timezone.utc).isoformat(),
        }
        json_payload = json.dumps(payload).encode('utf-8')
        return SecureSession._get_fernet().encrypt(json_payload).decode('utf-8')

    @staticmethod
    def verify(token: str) -> dict | None:
        try:
            decrypted_payload = SecureSession._get_fernet().decrypt(token.encode('utf-8'))
            payload = json.loads(decrypted_payload)
            if datetime.fromisoformat(payload["exp"]) < datetime.now(timezone.utc):
                logger.warning("Session verification failed: token expired.")
                return None
            return payload
        except (InvalidToken, json.JSONDecodeError, TypeError, KeyError) as e:
            logger.error(f"Session verification failed: {type(e).__name__} - {e}")
            return None

    @staticmethod
    def require_auth(f):
        @wraps(f)
        def decorated(*args, **kwargs):
            payload = None

            # 1. Try to get session from the secure cookie (for browser users)
            session_cookie = request.cookies.get("session_id")
            if session_cookie:
                payload = SecureSession.verify(session_cookie)

            # 2. If no cookie, try Authorization header (for curl/API users)
            if not payload:
                auth_header = request.headers.get("Authorization")
                if auth_header and auth_header.startswith("Bearer "):
                    google_token = auth_header.split(" ", 1)[1]
                    try:
                        # Validate the token with Google's tokeninfo endpoint
                        resp = httpx.get(f"https://oauth2.googleapis.com/tokeninfo?access_token={google_token}")
                        if resp.status_code == 200:
                            info = resp.json()

                            my_client_id = SecretManager.get_secret("oauth-client-id")
                            if info.get("aud") != my_client_id:
                                logger.warning(
                                    f"Token audience mismatch! Expected {my_client_id}, "
                                    f"but got {info.get('aud')}"
                                )
                                return jsonify({"error": "Invalid token audience"}), 401

                            # Construct a payload similar to our session
                            payload = {
                                "user_id": info.get("sub"),
                                "gaia_id": info.get("sub"),
                                "email": info.get("email"),
                                "name": info.get("name"),
                                "picture": info.get("picture"),
                                "access_token": google_token, # Pass the token through
                                "authenticated_via": "bearer_token"
                            }
                        else:
                            logger.warning(f"Bearer token validation failed: {resp.text}")
                    except Exception as e:
                        logger.error(f"Exception during bearer token validation: {e}")

            if not payload:
                return jsonify({"error": "Not authenticated or invalid token"}), 401

            request.user = payload
            return f(*args, **kwargs)
        return decorated

# --- Blueprint Definitions ---
auth_bp = Blueprint('auth', __name__, url_prefix='/auth')
api_bp = Blueprint('api', __name__, url_prefix='/api')

@auth_bp.route("/login")
def initiate_oauth():
    state = jwt.encode(
        {"exp": datetime.now(timezone.utc) + timedelta(minutes=10)},
        SecretManager.get_secret("oauth-jwt-secret"),
        algorithm="HS256"
    )
    params = {
        "client_id": SecretManager.get_secret("oauth-client-id"),
        "redirect_uri": Config.REDIRECT_URI,
        "response_type": "code",
        "scope": Config.SCOPE,
        "state": state,
        "access_type": "offline",
    }
    auth_url = f"{Config.GOOGLE_AUTH_URL}?{urlencode(params)}"
    return redirect(auth_url)

@auth_bp.route("/callback")
def handle_oauth_callback():
    code = request.args.get("code")
    state = request.args.get("state")

    try:
        jwt.decode(state, SecretManager.get_secret("oauth-jwt-secret"), algorithms=["HS256"])
    except jwt.InvalidTokenError:
        logger.warning("OAuth callback failed due to invalid CSRF state token.")
        return jsonify({"error": "Invalid state token (CSRF)"}), 403

    if not code:
        return jsonify({"error": "No authorization code received"}), 400

    try:
        token_data = {
            "code": code,
            "redirect_uri": Config.REDIRECT_URI,
            "grant_type": "authorization_code",
        }
        token_response = _google_api_request_with_retry(
            "POST", Config.GOOGLE_TOKEN_URL, data=token_data, timeout=10.0
        )
    except httpx.HTTPStatusError as e:
        logger.error(f"Failed to obtain access token after retries: {e.response.text}")
        return jsonify({"error": "Failed to exchange code for token"}), 500

    tokens = token_response.json()

    try:
        user_response = httpx.get(
            Config.GOOGLE_USERINFO_URL,
            headers={"Authorization": f"Bearer {tokens['access_token']}"},
            timeout=10.0,
        )
        user_response.raise_for_status()
    except httpx.HTTPStatusError as e:
        logger.error(f"Failed to fetch user info: {e.response.text}")
        return jsonify({"error": "Failed to fetch user info"}), 500

    user_data = user_response.json()

    # Correctly and robustly get the user's unique ID.
    # Google's OIDC conformant userinfo endpoint uses 'sub' (subject).
    # Some older or different Google APIs might return 'id'. We'll check both.
    user_unique_id = user_data.get("sub") or user_data.get("id")
    if not user_unique_id:
        logger.error("Could not find 'sub' or 'id' in Google userinfo response.")
        return jsonify({"error": "User ID not found in userinfo response"}), 500

    session_data = {
        "user_id": user_unique_id,
        "gaia_id": user_unique_id,
        "email": user_data.get("email"),
        "name": user_data.get("name"),
        "picture": user_data.get("picture"),
        "access_token": tokens.get("access_token"),
        "refresh_token": tokens.get("refresh_token"),
    }
    session_cookie = SecureSession.create(session_data, hours=24)

    response = make_response(redirect("/brush.html"))
    response.set_cookie(
        "session_id", session_cookie, httponly=True, secure=True,
        samesite="Lax", max_age=86400, path="/"
    )
    return response

@auth_bp.route("/logout", methods=["GET", "POST"])
def handle_logout():
    response = make_response(redirect("/") if request.method == "GET" else jsonify({"status": "logged_out"}))
    response.set_cookie("session_id", "", max_age=0)
    return response

@api_bp.route("/user")
@SecureSession.require_auth
def get_user_info():
    payload = request.user
    return jsonify({
        "authenticated": True,
        "user_id": payload.get("user_id"),
        "gaia_id": payload.get("gaia_id"),
        "email": payload.get("email"),
        "name": payload.get("name"),
        "picture": payload.get("picture"),
    })

@api_bp.route("/v1/trace_metrics", methods=["POST"])
@SecureSession.require_auth
def proxy_trace_metrics():
    """
    Proxies a request to the Brush API's trace_metrics endpoint using the
    user's stored server-side access token.
    """
    user_payload = request.user
    access_token = user_payload.get("access_token")

    if not access_token:
        return jsonify({"error": "No access token found in session"}), 401

    # You would define your actual Brush API URL here
    brush_api_url = 'https://brush-googleapis.corp.google.com/v1/trace_metrics'

    # Forward the JSON body from the original request
    request_data = request.get_json()
    if not request_data:
        return jsonify({"error": "Request body must be JSON"}), 400

    headers = {
        'Content-Type': 'application/json',
        'Authorization': f'Bearer {access_token}'
    }

    try:
        with httpx.Client() as client:
            response = client.post(brush_api_url, json=request_data, headers=headers)
            response.raise_for_status()
            return jsonify(response.json())
    except httpx.HTTPStatusError as e:
        logger.error(f"Brush API request failed: {e.response.text}")
        return jsonify({
            "error": "Failed to fetch from Brush API",
            "status_code": e.response.status_code,
            "details": e.response.text
        }), e.response.status_code
    except Exception as e:
        logger.error(f"An unexpected error occurred: {e}")
        return jsonify({"error": "An internal error occurred"}), 500

app.register_blueprint(auth_bp)
app.register_blueprint(api_bp)

# --- Global Error Handlers ---
@app.errorhandler(404)
def not_found_error(error):
    return jsonify({"error": "Not Found", "message": str(error)}), 404

@app.errorhandler(500)
def internal_error(error):
    logger.error(f"Server Error: {error}", exc_info=True)
    return jsonify({"error": "Internal Server Error"}), 500

@app.route("/")
def serve_index():
    return redirect("/brush.html")

@app.route("/health")
def health_check():
    return jsonify({"status": "healthy"})

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    app.run(host="0.0.0.0", port=port, debug=os.environ.get("DEBUG") == "1")
