# Open Grid Monitor Infrastructure

This directory contains the Docker-based infrastructure for collecting, storing, and visualizing grid frequency data from ESP32S3 devices.

## Architecture

- **Mosquitto MQTT Broker**: Receives data from ESP32S3 devices
- **Telegraf**: Consumes MQTT messages and forwards to InfluxDB
- **InfluxDB**: Time-series database for storing grid measurements
- **Grafana**: Visualization dashboard for monitoring grid data

## Setup

1. **Configure credentials**:
   ```bash
   cp .env.example .env
   # Edit .env with your credentials (ensure passwords are at least 8 characters)
   ```

2. **Run the setup script** (idempotent - safe to run multiple times):
   ```bash
   chmod +x setup.sh
   ./setup.sh
   ```

   **For a completely fresh installation** (removes all existing data):
   ```bash
   ./setup.sh --clean
   ```

The setup script will:
- Validate password requirements
- Create necessary directories
- Generate MQTT credentials
- Start Docker services
- Automatically generate InfluxDB API token
- Configure Telegraf with credentials
- Configure Grafana datasource and dashboard
- Provide access information

3. **Access the services**:
   - **Grafana**: http://localhost:3000 (pre-configured with Open Grid Monitor dashboard)
   - **InfluxDB**: http://localhost:8086  
   - **MQTT**: localhost:1883

## Features

### Pre-installed Grafana Dashboard
The setup includes a ready-to-use Grafana dashboard with:
- Real-time frequency monitoring with gauge and time series
- Voltage monitoring with proper thresholds  
- Device selection dropdown
- Configurable aggregation window
- Proper color coding for frequency (50Hz ±0.02Hz green zone)
- Voltage safety ranges (207-253V operational)

## MQTT Data Format

ESP32S3 devices should publish to different topics based on data type:

### Grid Measurements
**Topic**: `open_grid_monitor/{device_id}/measurement`
```json
{
  "timestamp": 1703123456789123,
  "frequency": 50.02,
  "voltage": 230.5,
  "current": 10.2,
  "power": 2351.0
}
```

### Device Status
**Topic**: `open_grid_monitor/{device_id}/status`
```json
{
  "timestamp": 1703123456789123,
  "rssi": -45,
  "free_heap": 234567
}
```

### Device Logs
**Topic**: `open_grid_monitor/{device_id}/logs`
```json
{
  "timestamp": 1703123456789123,
  "level": "INFO",
  "message": "Grid frequency measurement completed",
  "module": "ade7953"
}
```

## InfluxDB Structure

**Bucket**: `open_grid_monitor`
- **Measurement**: `grid_data` (frequency, voltage, current, power)
- **Measurement**: `device_status` (online, battery, rssi, heap)
- **Measurement**: `device_logs` (level, message, module)

**Common Tags**: `device_id`, `source`, `host`

## Services

- **Grafana**: http://localhost:3000 (includes pre-installed Open Grid Monitor dashboard)
- **InfluxDB**: http://localhost:8086 (automatically configured with API token)
- **MQTT**: localhost:1883 (password authentication enabled)
- **MQTT WebSocket**: localhost:9001

## Troubleshooting

### Grafana Datasource Issues
If you see "Only one datasource per organization can be marked as default" error:
- Ensure only one `.yml` file exists in `grafana/provisioning/datasources/`
- The template file should be in `grafana/influxdb-datasource-template.yml` (not in provisioning directory)
- Run `./setup.sh` again to regenerate configurations

### Services Not Starting
Check service status with:
```bash
docker-compose ps
docker-compose logs [service-name]
```

### Dashboard Not Loading
- Verify InfluxDB token is valid in Grafana datasource settings
- Check that data is being published to MQTT topics
- Ensure device_id matches what your ESP32S3 is publishing

### MQTT Connection Issues
- Verify MQTT credentials match your `.env` file
- Check mosquitto logs: `docker-compose logs mosquitto`
- Test connection: `mosquitto_pub -h localhost -p 1883 -u [username] -P [password] -t test -m "hello"`

## Security Notes

- Change default passwords in `.env`
- Consider using TLS certificates for production
- Restrict MQTT access using ACL rules
- Use secure tokens for InfluxDB API access
