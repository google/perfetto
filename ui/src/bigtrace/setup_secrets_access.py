import subprocess
import sys

# Configuration
SECRETS = [
    "oauth-client-secret",
    "oauth-client-id",
    "oauth-jwt-secret",
    "session-encryption-key"
]

def run_gcloud(args, exit_on_error=True):
    """Helper to run gcloud commands and print output."""
    try:
        # Run command and capture output
        result = subprocess.run(
            args,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {' '.join(args)}")
        print(e.stderr)
        if exit_on_error:
            sys.exit(1)
        return None

def main():
    # Get current configuration
    print("------------------------------------------------")
    print("Configuring environment...")
    project_id = run_gcloud(["gcloud", "config", "get-value", "project"])
    current_user = run_gcloud(["gcloud", "config", "get-value", "account"])

    print(f"Project: {project_id}")
    print(f"User: {current_user}")

    # ---------------------------------------------------------
    # Step 1: Grant User Secret Manager Admin Role
    # ---------------------------------------------------------
    print("------------------------------------------------")
    print(f"Granting Secret Manager Admin role to {current_user}...")

    run_gcloud([
        "gcloud", "projects", "add-iam-policy-binding", project_id,
        f"--member=user:{current_user}",
        "--role=roles/secretmanager.admin",
        "--condition=None"
    ])
    print("Success: Admin role granted.")

    # ---------------------------------------------------------
    # Step 2: Process Service Accounts
    # ---------------------------------------------------------
    # Get SAs from args or input
    if len(sys.argv) > 1:
        input_sas = sys.argv[1]
    else:
        print("------------------------------------------------")
        input_sas = input("Enter comma-separated service accounts: ")

    # Python makes string splitting much cleaner than Bash
    service_accounts = [sa.strip() for sa in input_sas.split(',') if sa.strip()]

    for sa in service_accounts:
        print("------------------------------------------------")
        print(f"Processing Service Account: {sa}")

        for secret in SECRETS:
            print(f"  -> Adding accessor role for secret: {secret}")

            # We don't exit on error here so one failure doesn't stop the whole loop
            run_gcloud([
                "gcloud", "secrets", "add-iam-policy-binding", secret,
                f"--project={project_id}",
                f"--member=serviceAccount:{sa}",
                "--role=roles/secretmanager.secretAccessor",
                "--quiet"
            ], exit_on_error=False)

            print("     Done.")

    print("------------------------------------------------")
    print("All operations complete.")

if __name__ == "__main__":
    main()
