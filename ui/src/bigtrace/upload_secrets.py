import json
import secrets
import sys
import os
from google.cloud import secretmanager
import google.auth
from google.api_core import exceptions

# Configuration
SECRETS_MAPPING = {
    "oauth-client-id": "client_id",
    "oauth-client-secret": "client_secret",
}
ENCRYPTION_SECRET_NAME = "session-encryption-key"
JWT_SECRET_NAME = "oauth-jwt-secret"  # For signing JWTs, separate from encryption
DEFAULT_CLIENTS_FILE = "clients.json"

def get_project_id():
    """Get the current Google Cloud Project ID."""
    try:
        _, project_id = google.auth.default()
    except google.auth.exceptions.DefaultCredentialsError:
        print("Error: Could not determine credentials. Run 'gcloud auth application-default login' first.")
        sys.exit(1)

    if not project_id:
        print("Error: Could not determine Project ID. Run 'gcloud config set project <PROJECT_ID>'")
        sys.exit(1)
    return project_id

def create_or_update_secret(client, project_id, secret_id, payload):
    """Creates a secret if missing, then adds a new version."""
    parent = f"projects/{project_id}"
    secret_path = f"{parent}/secrets/{secret_id}"

    # 1. Create the secret if it doesn't exist
    try:
        client.get_secret(request={"name": secret_path})
        print(f"✓ Secret '{secret_id}' exists.")
    except Exception:
        print(f"  Creating secret '{secret_id}'...")
        try:
            client.create_secret(
                request={
                    "parent": parent,
                    "secret_id": secret_id,
                    "secret": {"replication": {"automatic": {}}},
                }
            )
        except exceptions.PermissionDenied as e:
            # Catch the specific "API Disabled" error
            if "Secret Manager API" in str(e) and "disabled" in str(e):
                print(f"\n❌ Error: The Secret Manager API is disabled on project '{project_id}'.")
                print("   Run the following command to enable it:")
                print(f"   gcloud services enable secretmanager.googleapis.com --project {project_id}\n")
                sys.exit(1)
            # Catch standard permission errors (IAM)
            elif "Permission denied" in str(e):
                print(f"\n❌ Error: Permission denied on project '{project_id}'.")
                print("   Ensure you are logged in and have the 'Secret Manager Admin' role.")
                print(f"   Try: gcloud auth application-default login\n")
                sys.exit(1)
            raise e

    # 2. Add the secret version
    print(f"  Adding new version to '{secret_id}'...")
    payload_bytes = payload.encode("UTF-8")
    client.add_secret_version(
        request={
            "parent": secret_path,
            "payload": {"data": payload_bytes},
        }
    )

def main():
    print("--- 🔐 OAuth Secret Uploader ---")

    # 1. Determine file path from args or default
    if len(sys.argv) > 1:
        clients_file = sys.argv[1]
    else:
        clients_file = DEFAULT_CLIENTS_FILE

    print(f"Reading configuration from: {clients_file}")

    # 2. Load clients.json (or specified file)
    try:
        if not os.path.exists(clients_file):
            print(f"Error: File '{clients_file}' not found.")
            print(f"Usage: python {sys.argv[0]} [path/to/clients.json]")
            sys.exit(1)

        with open(clients_file, "r") as f:
            data = json.load(f)

        # Google JSONs are usually wrapped in "web" or "installed"
        if "web" in data:
            creds = data["web"]
        elif "installed" in data:
            creds = data["installed"]
        else:
            print("Error: JSON must contain a 'web' or 'installed' key.")
            sys.exit(1)

    except json.JSONDecodeError:
        print(f"Error: Failed to parse JSON from '{clients_file}'.")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

    # 3. Setup Client
    project_id = get_project_id()
    print(f"Target Project: {project_id}")
    client = secretmanager.SecretManagerServiceClient()

    # 4. Upload Client ID and Secret
    for secret_name, json_key in SECRETS_MAPPING.items():
        if json_key not in creds:
            print(f"Error: Key '{json_key}' not found in configuration file.")
            continue

        value = creds[json_key]
        create_or_update_secret(client, project_id, secret_name, value)

    # 5. Generate and Upload JWT Secret (if requested)
    print(f"\nChecking {JWT_SECRET_NAME}...")
    try:
        # Check if a version already exists to avoid overwriting a production key
        latest = f"projects/{project_id}/secrets/{JWT_SECRET_NAME}/versions/latest"
        client.access_secret_version(request={"name": latest})
        print(f"✓ {JWT_SECRET_NAME} already has a version. Skipping generation.")
    except Exception:
        print(f"  Generating new secure random key for {JWT_SECRET_NAME}...")
        random_key = secrets.token_urlsafe(64)
        create_or_update_secret(client, project_id, JWT_SECRET_NAME, random_key)

    # 6. Generate and Upload Session Encryption Key if it doesn't exist
    print(f"\nChecking for {ENCRYPTION_SECRET_NAME}...")
    secret_path = f"projects/{project_id}/secrets/{ENCRYPTION_SECRET_NAME}"
    try:
        client.get_secret(request={"name": secret_path})
        print(f"✓ Secret '{ENCRYPTION_SECRET_NAME}' already exists. No action needed.")
    except exceptions.NotFound:
        from cryptography.fernet import Fernet
        print(f"  Secret '{ENCRYPTION_SECRET_NAME}' not found. Generating a new Fernet key...")
        fernet_key = Fernet.generate_key()
        create_or_update_secret(client, project_id, ENCRYPTION_SECRET_NAME, fernet_key.decode())

    print("\n✓ All secrets processed successfully.")

if __name__ == "__main__":
    main()
