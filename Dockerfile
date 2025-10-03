# Stage 1: Build the Perfetto UI
FROM node:18 AS builder

# Install utilities including jq
RUN apt-get update && apt-get install -y python3-venv jq && rm -rf /var/lib/apt/lists/*

# Clone the repository inside the builder
RUN git clone https://github.com/google/perfetto.git /app
WORKDIR /app

# Navigate to the UI directory to modify package.json
WORKDIR /app/ui

# Remove the devDependency that uses the "link:" protocol
# This prevents issues with 'pnpm install' run by the build-deps script.
RUN jq 'del(.devDependencies."eslint-plugin-perfetto")' package.json > package.json.tmp && mv package.json.tmp package.json

# Go back to the root directory
WORKDIR /app

# Install UI dependencies using the script. This runs 'pnpm install'.
RUN tools/install-build-deps --ui

# Build the UI. Default output is /app/out/ui/ui/dist
RUN node ui/build.js

# Stage 2: Serve the static files using nginx
FROM nginx:alpine
RUN rm -rf /usr/share/nginx/html/*

# Copy the entire contents of the dist directory, including
# index.html at the root and the versioned asset folder.
COPY --from=builder /app/out/ui/ui/dist /usr/share/nginx/html

EXPOSE 80
CMD ["nginx", "-g", "daemon off;"]
