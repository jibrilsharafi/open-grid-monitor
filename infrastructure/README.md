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
   # Edit .env with your credentials
   ```

2. **Run the setup script**:
   ```bash
   chmod +x setup.sh
   ./setup.sh
   ```

3. **Start the services**:
   ```bash
   docker-compose up -d
   ```

4. **Configure InfluxDB token** (after first startup):
   - Visit http://localhost:8086
   - Login with your admin credentials
   - Go to Data > API Tokens
   - Generate a new token with read/write access to the `open_grid_monitor` bucket
   - Update `INFLUXDB_TOKEN` in your `.env` file
   - Run `./setup.sh` again to update Telegraf configuration
   - Restart Telegraf: `docker-compose restart telegraf-open-grid-monitor`

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

**Bucket**: `open-grid-monitor`
- **Measurement**: `grid_data` (frequency, voltage, current, power)
- **Measurement**: `device_status` (online, battery, rssi, heap)
- **Measurement**: `device_logs` (level, message, module)

**Common Tags**: `device_id`, `source`, `host`

## Services

- **Grafana**: http://localhost:3000
- **InfluxDB**: http://localhost:8086
- **MQTT**: localhost:1883
- **MQTT WebSocket**: localhost:9001

## Security Notes

- Change default passwords in `.env`
- Consider using TLS certificates for production
- Restrict MQTT access using ACL rules
- Use secure tokens for InfluxDB API access
