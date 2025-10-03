# Stage 1: Build the Perfetto UI from local source
FROM node:18 AS builder

# Install utilities including jq
RUN apt-get update && apt-get install -y python3-venv jq && rm -rf /var/lib/apt/lists/*

# Set workdir and copy the entire local project content
WORKDIR /app
COPY . .

# Navigate to the UI directory to modify package.json
WORKDIR /app/ui

# Remove the devDependency that uses the "link:" protocol
RUN jq 'del(.devDependencies."eslint-plugin-perfetto")' package.json > package.json.tmp && mv package.json.tmp package.json

# Go back to the root directory
WORKDIR /app

# Remove the existing pre-push hook to prevent FileExistsError before creating the symlink.
RUN rm -f /app/.git/hooks/pre-push

# Install UI dependencies using the script.
RUN tools/install-build-deps --ui

# Remove the existing gen symlink to prevent EEXIST error.
RUN rm -rf /app/ui/src/gen

# Build the UI.
RUN node ui/build.js --bigtrace

# Stage 2: Serve the static files using nginx
FROM nginx:alpine
# Install Python and pip
RUN apk --no-cache add python3 py3-pip

WORKDIR /app

# Copy and install Python dependencies
COPY ui/src/bigtrace/requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt

# Copy static assets from the builder stage
COPY --from=builder /app/out/ui/ui/dist /usr/share/nginx/html

# Copy the OAuth server and nginx config
COPY ui/src/bigtrace/oauth_redirect_server.py /app/main.py
COPY nginx.conf /etc/nginx/nginx.conf

# Copy the entrypoint script and make it executable
COPY ui/src/bigtrace/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

EXPOSE 80

# Use the entrypoint script to start the services.
# This exec form allows the container to receive signals correctly.
CMD ["/app/entrypoint.sh"]
