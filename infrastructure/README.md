# Infrastructure

This directory contains the Docker Compose setup for the Open Grid Monitor backend services.

![Grafana dashboard](../resources/grafana.png)

## Automated Setup

The infrastructure is designed for **one-click deployment** with automated configuration:

- **Pre-built dashboard** ready to visualize your power data
- **Automatic credential management** for all services
- **Self-configuring data pipeline** from MQTT to visualization
- **No manual setup required** - just run the setup script

Use `./setup.sh` instead of plain `docker-compose up` for a complete automated setup that handles authentication, tokens, and service configuration.

## Services

- **InfluxDB** - Time series database for storing power measurements
- **Grafana** - Visualization dashboard 
- **Mosquitto** - MQTT broker for device communication
- **Telegraf** - Data collection agent that bridges MQTT to InfluxDB

## Quick Start

1. Copy environment variables:
   ```bash
   cp .env.example .env
   ```

2. Edit `.env` with your credentials

3. **Run the automated setup** (recommended):
   ```bash
   ./setup.sh
   ```
   
   This handles everything automatically: credentials, tokens, dashboard provisioning, and service configuration.

   *Alternative: Manual docker-compose (requires manual configuration):*
   ```bash
   docker-compose up -d
   ```

## Access

- Grafana: http://localhost:3000
- InfluxDB: http://localhost:8086
- MQTT: localhost:1883

## Data Flow

ESP32 devices → MQTT → Telegraf → InfluxDB → Grafana

## Configuration

- `mosquitto/config/` - MQTT broker settings and ACL
- `telegraf/` - Data collection configuration
- `grafana-provisioning/` - Dashboard and datasource setup

## Troubleshooting

Check service logs:
```bash
docker-compose logs [service_name]
```

Restart a service:
```bash
docker-compose restart [service_name]
```