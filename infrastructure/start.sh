#!/bin/bash

# Simple start script for Open Grid Monitor infrastructure
# Use this after initial setup is complete

echo "🚀 Starting Open Grid Monitor infrastructure..."

# Check if .env file exists
if [ ! -f .env ]; then
    echo "❌ Error: .env file not found! Run setup.sh first."
    exit 1
fi

# Check if runtime config exists
if [ ! -f telegraf/telegraf-open-grid-monitor-runtime.conf ]; then
    echo "❌ Error: Telegraf runtime config not found! Run setup.sh first."
    exit 1
fi

# Start services
docker-compose up -d

echo ""
echo "✅ Services starting..."
echo ""
echo "📊 Access your services at:"
echo "  - Grafana: http://localhost:3000"
echo "  - InfluxDB: http://localhost:8086" 
echo "  - MQTT: localhost:1883"
echo ""
echo "📝 Check status: docker-compose ps"
echo "📋 View logs: docker-compose logs -f"
