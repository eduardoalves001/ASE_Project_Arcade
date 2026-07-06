const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

const app = express();
const server = http.createServer(app);
const io = new Server(server);

/* Commands the dashboard is allowed to forward to the console. */
const allowedCommands = new Set([
    'menu', 'start_flappy', 'start_pong', 'start_dino', 'select', 'reset_scores'
]);

const configPath = path.join(__dirname, 'mqtt-config.json');

function loadMqttConfig() {
    let fileConfig = {};
    if (fs.existsSync(configPath)) {
        fileConfig = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    }
    return {
        brokerUrl: process.env.MQTT_BROKER_URL || fileConfig.brokerUrl || 'mqtts://127.0.0.1:8883',
        caFile: process.env.MQTT_CA_FILE || fileConfig.caFile || path.join(__dirname, '..', 'main', 'mqtt_broker_ca.pem'),
        username: process.env.MQTT_USERNAME || fileConfig.username || 'arcade',
        password: process.env.MQTT_PASSWORD || fileConfig.password || 'arcade-local-2026'
    };
}

const mqttConfig = loadMqttConfig();
if (!path.isAbsolute(mqttConfig.caFile)) {
    mqttConfig.caFile = path.resolve(__dirname, mqttConfig.caFile);
}

const mqttOptions = {
    protocolVersion: 4,
    reconnectPeriod: 2000,
    username: mqttConfig.username,
    password: mqttConfig.password
};

if (mqttConfig.brokerUrl.startsWith('mqtts://')) {
    mqttOptions.rejectUnauthorized = true;
    mqttOptions.ca = fs.readFileSync(mqttConfig.caFile);
}

const mqttClient = mqtt.connect(mqttConfig.brokerUrl, mqttOptions);

mqttClient.on('connect', () => {
    console.log(`Backend connected to MQTT broker: ${mqttConfig.brokerUrl}`);
    mqttClient.subscribe('arcade/status');
});

mqttClient.on('error', (error) => {
    console.error('MQTT connection error:', error.message);
});

mqttClient.on('message', (topic, message) => {
    if (topic === 'arcade/status') {
        try {
            const data = JSON.parse(message.toString());
            io.emit('arcade_status', data);
        } catch (e) {
            console.error('Failed to parse MQTT JSON:', e);
        }
    }
});

app.use(express.static(path.join(__dirname, 'public')));

io.on('connection', (socket) => {
    console.log('Browser connected to the dashboard');
    socket.on('command', (cmd) => {
        const action = cmd && cmd.action;
        if (allowedCommands.has(action)) {
            console.log(`Forwarding "${action}" to the console over MQTT.`);
            mqttClient.publish('arcade/command', action);
        }
    });
});

const PORT = 3000;
server.listen(PORT, '0.0.0.0', () => {
    console.log(`ESP-Arcade dashboard running at: http://localhost:${PORT}`);
});
