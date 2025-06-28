#!/bin/bash

# Setup script for Open Grid Monitor infrastructure
# This script initializes the MQTT password file, starts services, and configures InfluxDB token
# 
# This script is idempotent - it can be run multiple times safely and will only make
# necessary changes, skipping steps that are already completed correctly.
#
# Usage:
#   ./setup.sh          - Normal setup (idempotent)
#   ./setup.sh --clean   - Clean all data and start fresh
#   ./setup.sh --reset   - Same as --clean

# Check for clean/reset flag
CLEAN_INSTALL=false
if [[ "$1" == "--clean" ]] || [[ "$1" == "--reset" ]]; then
    CLEAN_INSTALL=true
    echo "🧹 CLEAN INSTALL MODE - This will delete ALL existing data!"
    echo ""
    echo "This will remove:"
    echo "  - All Grafana data (dashboards, users, settings)"
    echo "  - All InfluxDB data (measurements, tokens, buckets)"
    echo "  - All MQTT data (users, message history)"
    echo "  - All generated configuration files"
    echo ""
    read -p "Are you sure you want to continue? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 1
    fi
    echo ""
fi

echo "🚀 Setting up Open Grid Monitor infrastructure..."

# Check if .env file exists
if [ ! -f .env ]; then
    echo "❌ Error: .env file not found!"
    echo "Please copy .env.example to .env and configure your credentials:"
    echo "  cp .env.example .env"
    echo "  # Edit .env with your credentials"
    exit 1
fi

# Source environment variables
source .env

# Clean installation if requested
if [ "$CLEAN_INSTALL" = true ]; then
    echo "🧹 Cleaning all existing data..."
    
    # Stop all services first
    echo "  - Stopping Docker services..."
    docker-compose down 2>/dev/null || true
    
    # Remove all persistent data directories
    echo "  - Removing Grafana data..."
    rm -rf grafana/grafana.db grafana/csv grafana/pdf grafana/png 2>/dev/null || true
    
    echo "  - Removing InfluxDB data..."
    rm -rf influxdb/* 2>/dev/null || true
    
    echo "  - Removing MQTT data..."
    rm -rf mosquitto/data/* mosquitto/log/* 2>/dev/null || true
    
    # Remove generated configuration files
    echo "  - Removing generated configurations..."
    rm -f telegraf/telegraf-open-grid-monitor-runtime.conf 2>/dev/null || true
    rm -f grafana/provisioning/datasources/influxdb.yml 2>/dev/null || true
    rm -f mosquitto/config/passwd 2>/dev/null || true
    
    # Reset InfluxDB token in .env file
    echo "  - Resetting InfluxDB token in .env..."
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS
        sed -i '' "s/INFLUXDB_TOKEN=.*/INFLUXDB_TOKEN=your_influxdb_token_here/" .env
    else
        # Linux
        sed -i "s/INFLUXDB_TOKEN=.*/INFLUXDB_TOKEN=your_influxdb_token_here/" .env
    fi
    
    # Reload environment variables
    source .env
    
    echo "✅ Clean installation prepared - starting fresh setup..."
    echo ""
fi

# Validate password requirements
echo "🔒 Validating password requirements..."

validate_password() {
    local password="$1"
    local name="$2"
    
    if [ ${#password} -lt 8 ]; then
        echo "❌ Error: ${name} must be at least 8 characters long!"
        echo "   Current length: ${#password} characters"
        echo "   Please update your .env file with a stronger password."
        return 1
    fi
    return 0
}

# Check all passwords
if ! validate_password "$MQTT_PASSWORD" "MQTT_PASSWORD"; then exit 1; fi
if ! validate_password "$INFLUXDB_ADMIN_PASSWORD" "INFLUXDB_ADMIN_PASSWORD"; then exit 1; fi
if ! validate_password "$GRAFANA_ADMIN_PASSWORD" "GRAFANA_ADMIN_PASSWORD"; then exit 1; fi

echo "✅ All passwords meet minimum requirements!"

# Create data directories
echo "📁 Creating data directories..."
mkdir -p mosquitto/data
mkdir -p mosquitto/log
mkdir -p influxdb
mkdir -p grafana
mkdir -p grafana/provisioning/datasources
mkdir -p grafana/provisioning/dashboards
mkdir -p grafana/dashboards
mkdir -p telegraf

# Generate MQTT password file
echo "🔐 Creating MQTT user credentials..."

# Only regenerate if password file doesn't exist or credentials changed
if [ ! -f mosquitto/config/passwd ] || ! echo "${MQTT_USERNAME}:${MQTT_PASSWORD}" | docker run --rm -i --user root -v $(pwd)/mosquitto/config:/mosquitto/config eclipse-mosquitto:latest mosquitto_passwd -U /dev/stdin > /dev/null 2>&1; then
    echo "${MQTT_USERNAME}:${MQTT_PASSWORD}" > mosquitto/config/passwd_temp
    
    # Fix permissions and ownership to avoid warnings
    chmod 600 mosquitto/config/passwd_temp
    
    # Use docker to change ownership to root inside container context
    docker run --rm --user root -v $(pwd)/mosquitto/config:/mosquitto/config alpine:latest chown root:root /mosquitto/config/passwd_temp
    
    # Use mosquitto_passwd to hash the password
    docker run --rm --user root -v $(pwd)/mosquitto/config:/mosquitto/config eclipse-mosquitto:latest mosquitto_passwd -U /mosquitto/config/passwd_temp
    
    # Move the hashed password file to the correct location
    mv mosquitto/config/passwd_temp mosquitto/config/passwd
    echo "✅ MQTT credentials updated"
else
    echo "✅ MQTT credentials already up to date"
fi

# Pre-create Telegraf configuration file to avoid Docker mount issues
echo "⚙️  Pre-creating Telegraf configuration..."

# Ensure telegraf runtime config is not a directory (cleanup any previous errors)
if [ -d telegraf/telegraf-open-grid-monitor-runtime.conf ]; then
    rm -rf telegraf/telegraf-open-grid-monitor-runtime.conf
fi

# Create a placeholder configuration file (will be updated later with actual credentials)
if [ ! -f telegraf/telegraf-open-grid-monitor-runtime.conf ]; then
    cp telegraf/telegraf-open-grid-monitor.conf telegraf/telegraf-open-grid-monitor-runtime.conf
    echo "✅ Telegraf placeholder configuration created"
fi

echo "🐳 Starting Docker services..."
docker-compose up -d

# Wait for InfluxDB to be ready
echo "⏳ Waiting for InfluxDB to be ready..."
until docker exec influxdb influx ping > /dev/null 2>&1; do
    echo "   Waiting for InfluxDB..."
    sleep 2
done
echo "✅ InfluxDB is ready!"

# Check if token is already set and valid
if [ "${INFLUXDB_TOKEN}" != "your_influxdb_token_here" ]; then
    echo "🔑 InfluxDB token already configured, testing validity..."
    if docker exec influxdb influx auth list --token "${INFLUXDB_TOKEN}" > /dev/null 2>&1; then
        echo "✅ Existing token is valid!"
        TOKEN_VALID=true
    else
        echo "⚠️  Existing token is invalid, generating new one..."
        TOKEN_VALID=false
    fi
else
    echo "🔑 Generating InfluxDB API token..."
    TOKEN_VALID=false
fi

if [ "$TOKEN_VALID" != "true" ]; then
    # First, authenticate with InfluxDB using admin credentials
    echo "🔐 Authenticating with InfluxDB..."
    
    # Check if config already exists, delete it if it does
    if docker exec influxdb influx config list | grep -q "default"; then
        echo "   Removing existing config..."
        docker exec influxdb influx config rm default
    fi
    
    # Create an auth config to authenticate CLI commands
    docker exec influxdb influx config create \
        --config-name default \
        --host-url http://localhost:8086 \
        --org "${INFLUXDB_ORG}" \
        --username-password "${INFLUXDB_ADMIN_USERNAME}:${INFLUXDB_ADMIN_PASSWORD}" \
        --active
    
    # Wait a moment for authentication to settle
    sleep 2
    
    # List available buckets to find our bucket ID
    echo "🔍 Looking up bucket ID for '${INFLUXDB_BUCKET}'..."
    BUCKET_ID=$(docker exec influxdb influx bucket list --json 2>/dev/null | jq -r '.[] | select(.name == "'"${INFLUXDB_BUCKET}"'") | .id' 2>/dev/null || echo "")
    
    if [ -z "$BUCKET_ID" ]; then
        # Fallback to grep if jq is not available
        BUCKET_ID=$(docker exec influxdb influx bucket list --json 2>/dev/null | grep -A 10 "\"name\":\"${INFLUXDB_BUCKET}\"" | grep -o '"id":"[^"]*"' | cut -d'"' -f4)
    fi
    
    if [ -z "$BUCKET_ID" ]; then
        echo "❌ Bucket '${INFLUXDB_BUCKET}' not found! This shouldn't happen as it should be created during initialization."
        echo "📋 Available buckets:"
        docker exec influxdb influx bucket list
        exit 1
    fi
    
    echo "✅ Found bucket ID: ${BUCKET_ID}"
    
    echo "🔑 Creating API token..."
    TOKEN_RESPONSE=$(docker exec influxdb influx auth create \
        --read-bucket "${BUCKET_ID}" \
        --write-bucket "${BUCKET_ID}" \
        --description "Open Grid Monitor Token" \
        --json 2>&1)
    
    echo "Token creation response: ${TOKEN_RESPONSE}"
    
    # Extract token from JSON response (handle formatted JSON)
    NEW_TOKEN=$(echo "${TOKEN_RESPONSE}" | jq -r '.token' 2>/dev/null || echo "${TOKEN_RESPONSE}" | grep -o '"token": *"[^"]*"' | cut -d'"' -f4)
    
    if [ -n "$NEW_TOKEN" ] && [ "$NEW_TOKEN" != "null" ]; then
        echo "✅ Generated new InfluxDB token!"
        
        # Update .env file with new token
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS
            sed -i '' "s/INFLUXDB_TOKEN=.*/INFLUXDB_TOKEN=${NEW_TOKEN}/" .env
        else
            # Linux
            sed -i "s/INFLUXDB_TOKEN=.*/INFLUXDB_TOKEN=${NEW_TOKEN}/" .env
        fi
        
        echo "✅ Updated .env file with new token!"
        
        # Reload environment variables
        source .env
    else
        echo "❌ Failed to extract token from response!"
        echo "Full response: ${TOKEN_RESPONSE}"
        echo "Please check InfluxDB logs for more details."
        exit 1
    fi
fi

# Create Telegraf configuration with actual credentials
echo "⚙️  Updating Telegraf configuration with credentials..."

# Ensure telegraf runtime config is not a directory (cleanup any previous errors)
if [ -d telegraf/telegraf-open-grid-monitor-runtime.conf ]; then
    rm -rf telegraf/telegraf-open-grid-monitor-runtime.conf
fi

# Update configuration with actual credentials
sed -e "s/MQTT_USERNAME_PLACEHOLDER/${MQTT_USERNAME}/g" \
    -e "s/MQTT_PASSWORD_PLACEHOLDER/${MQTT_PASSWORD}/g" \
    -e "s/INFLUXDB_TOKEN_PLACEHOLDER/${INFLUXDB_TOKEN}/g" \
    -e "s/INFLUXDB_ORG_PLACEHOLDER/${INFLUXDB_ORG}/g" \
    -e "s/INFLUXDB_BUCKET_PLACEHOLDER/${INFLUXDB_BUCKET}/g" \
    telegraf/telegraf-open-grid-monitor.conf > telegraf/telegraf-open-grid-monitor-runtime.conf

echo "✅ Telegraf configuration updated with credentials"

# Restart Telegraf to pick up new configuration
echo "🔄 Restarting Telegraf with new configuration..."
docker-compose restart telegraf-open-grid-monitor

# Create Grafana datasource configuration with actual credentials
echo "📊 Configuring Grafana datasource with credentials..."

# Only regenerate if runtime config doesn't exist or template has changed
if [ ! -f grafana/provisioning/datasources/influxdb.yml ] || [ grafana/influxdb-datasource-template.yml -nt grafana/provisioning/datasources/influxdb.yml ]; then
    sed -e "s/INFLUXDB_TOKEN_PLACEHOLDER/${INFLUXDB_TOKEN}/g" \
        -e "s/INFLUXDB_ORG_PLACEHOLDER/${INFLUXDB_ORG}/g" \
        -e "s/INFLUXDB_BUCKET_PLACEHOLDER/${INFLUXDB_BUCKET}/g" \
        grafana/influxdb-datasource-template.yml > grafana/provisioning/datasources/influxdb.yml
    
    echo "✅ Grafana datasource configuration updated"
    
    # Restart Grafana to pick up new configuration
    echo "🔄 Restarting Grafana with new configuration..."
    docker-compose restart grafana
else
    echo "✅ Grafana datasource configuration already up to date"
fi

echo ""
echo "🎉 Setup complete!"
echo ""
echo "📊 Services are available at:"
echo "  - Grafana: http://localhost:3000 (admin/${GRAFANA_ADMIN_PASSWORD})"
echo "    └─ Open Grid Monitor dashboard is pre-installed and ready to use!"
echo "  - InfluxDB: http://localhost:8086 (${INFLUXDB_ADMIN_USERNAME}/${INFLUXDB_ADMIN_PASSWORD})"
echo "  - MQTT: localhost:1883 (${MQTT_USERNAME}/${MQTT_PASSWORD})"
echo "  - MQTT WebSocket: localhost:9001"
echo ""
echo "🗂  InfluxDB Configuration:"
echo "  - Organization: ${INFLUXDB_ORG}"
echo "  - Bucket: ${INFLUXDB_BUCKET}"
echo "  - Token: ${INFLUXDB_TOKEN:0:20}..."
echo ""
echo "📡 MQTT Topics for ESP32S3:"
echo "  - Grid data: open_grid_monitor/{device_id}/measurement"
echo "  - Device status: open_grid_monitor/{device_id}/status"
echo "  - Device logs: open_grid_monitor/{device_id}/logs"
echo ""
echo "📊 Dashboard Information:"
echo "  - Dashboard: 'Open Grid Monitor' is pre-installed in Grafana"
echo "  - Features: Frequency/voltage monitoring with safety thresholds"
echo "  - Device selection: Use the dropdown to filter by device_id"
echo "  - Data retention: Configured for real-time monitoring"
echo ""
echo "🔧 Next Steps:"
echo "  1. Configure your ESP32S3 device with MQTT credentials"
echo "  2. Start publishing data to the MQTT topics above"
echo "  3. Visit Grafana to see real-time grid monitoring"
echo ""
echo "💡 Setup Script Usage:"
echo "  ./setup.sh         - Normal setup (idempotent)"
echo "  ./setup.sh --clean  - Clean all data and start fresh"
echo "  ./setup.sh --reset  - Same as --clean"
echo ""
echo "🔍 Verifying services..."

# Quick health check
echo -n "  - Grafana health: "
if curl -s -f http://localhost:3000/api/health > /dev/null 2>&1; then
    echo "✅ OK"
else
    echo "⚠️  Service starting (may take a few moments)"
fi

echo -n "  - InfluxDB health: "
if docker exec influxdb influx ping > /dev/null 2>&1; then
    echo "✅ OK"
else
    echo "⚠️  Service starting (may take a few moments)"
fi

echo ""
echo "🚀 Your Open Grid Monitor infrastructure is ready!"
