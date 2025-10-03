import sys
import subprocess
import google.auth
from google.api_core import exceptions

def get_project_id():
    """Gets the default GCP project ID."""
    try:
        _, project_id = google.auth.default()
        if not project_id:
            raise RuntimeError("Could not determine project ID.")
        return project_id
    except (google.auth.exceptions.DefaultCredentialsError, RuntimeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        print("Please configure your gcloud credentials and project by running:", file=sys.stderr)
        print("  gcloud auth application-default login", file=sys.stderr)
        print("  gcloud config set project YOUR_PROJECT_ID", file=sys.stderr)
        sys.exit(1)

def run_gcloud_command(command, description):
    """Runs a gcloud command and handles errors."""
    print(f"- {description}... ", end="", flush=True)
    try:
        subprocess.run(command, check=True, capture_output=True, text=True)
        print("✓")
    except subprocess.CalledProcessError as e:
        print("✗ FAILED")
        print("\n--- gcloud Error ---", file=sys.stderr)
        print(e.stderr, file=sys.stderr)
        print("--------------------", file=sys.stderr)
        print("\nPlease check the error message above.", file=sys.stderr)
        sys.exit(1)

def get_current_user_email():
    """Gets the email of the currently authenticated gcloud user."""
    try:
        result = subprocess.run(
            ["gcloud", "config", "get-value", "account"],
            check=True, capture_output=True, text=True
        )
        return result.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None

def main():
    """Main function to set up Cloud Build permissions."""
    print("--- ☁️  Cloud Build Setup Script ---")
    project_id = get_project_id()
    print(f"Target Project: {project_id}\n")

    # --- Step 1: Initial Setup ---
    run_gcloud_command(
        ["gcloud", "services", "enable", "cloudbuild.googleapis.com", f"--project={project_id}"],
        "Enabling Cloud Build API"
    )
    
    print("- Getting project number... ", end="", flush=True)
    try:
        project_number_result = subprocess.run(
            ["gcloud", "projects", "describe", project_id, "--format=value(projectNumber)"],
            check=True, capture_output=True, text=True
        )
        project_number = project_number_result.stdout.strip()
        print(f"✓ ({project_number})")
    except subprocess.CalledProcessError:
        print("✗ FAILED to get project number.")
        sys.exit(1)

    # --- Step 2: Configure Service Account Variables ---
    cloud_build_sa = f"{project_number}@cloudbuild.gserviceaccount.com"
    cloud_run_sa = f"{project_number}-compute@developer.gserviceaccount.com"
    
    print(f"\n- Cloud Build Service Account: {cloud_build_sa}")
    print(f"- Cloud Run Service Account:   {cloud_run_sa}\n")

    # --- Step 3: Grant Permissions to the User ---
    print("--- Granting Permissions to User ---")
    user_email = get_current_user_email()
    if user_email:
        user_member = f"user:{user_email}"
        run_gcloud_command(
            ["gcloud", "projects", "add-iam-policy-binding", project_id, f"--member={user_member}", "--role=roles/iam.serviceAccountAdmin"],
            f"Granting 'Service Account Admin' to you ({user_email})"
        )
        run_gcloud_command(
            ["gcloud", "projects", "add-iam-policy-binding", project_id, f"--member={user_member}", "--role=roles/viewer"],
            f"Granting 'Viewer' to you ({user_email})"
        )
    else:
        print("⚠️ Could not determine current gcloud user. Skipping user role grants.")

    # --- Step 4: Grant All Necessary Permissions to BOTH Service Accounts ---
    print("\n--- Granting All Necessary Permissions to Service Accounts ---")
    
    service_accounts = {
        "Cloud Build SA": cloud_build_sa,
        "Cloud Run SA": cloud_run_sa
    }
    
    roles_to_grant = {
        "roles/run.developer": "Cloud Run Developer",
        "roles/iam.serviceAccountUser": "Service Account User",
        "roles/artifactregistry.writer": "Artifact Registry Writer",
        "roles/storage.objectViewer": "Storage Object Viewer",
        "roles/logging.logWriter": "Logs Writer",
        "roles/cloudbuild.builds.builder": "Cloud Build Builder"
    }

    for sa_name, sa_email in service_accounts.items():
        print(f"\nGranting roles to {sa_name} ({sa_email}):")
        sa_member = f"serviceAccount:{sa_email}"
        for role, description in roles_to_grant.items():
            run_gcloud_command(
                ["gcloud", "projects", "add-iam-policy-binding", project_id, f"--member={sa_member}", f"--role={role}"],
                f"Granting '{description}'"
            )

    # Also grant the Cloud Build SA permission to impersonate the Cloud Run SA.
    print(f"\nGranting special impersonation permission:")
    run_gcloud_command(
        [
            "gcloud", "iam", "service-accounts", "add-iam-policy-binding",
            cloud_run_sa, f"--project={project_id}", f"--member=serviceAccount:{cloud_build_sa}",
            "--role=roles/iam.serviceAccountUser"
        ],
        "Granting Cloud Build SA permission to act as Cloud Run SA"
    )

    print("\n✅ Cloud Build setup complete.")
    print("You can now deploy your application using the command:")
    print("  gcloud builds submit --config cloudbuild.yaml --no-source")


if __name__ == "__main__":
    main()