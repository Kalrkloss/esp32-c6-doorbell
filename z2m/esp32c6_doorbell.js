// Zigbee2MQTT external converter for the DIY ESP32-C6 doorbell push button.
//
// The device reports MultistateInput.PresentValue: 1 = contact closed (press),
// 0 = released. Every press becomes an `action` event ("single"), which is what
// Home Assistant shows as a device trigger - i.e. a doorbell button, not a
// switch entity.
//
// zigbeeModel/manufacturer must match kModelIdentifier/kManufacturerName in
// main/doorbell_config.hpp (C6-DOORBELL / DIY).

const exposes = require('zigbee-herdsman-converters/lib/exposes');

const definition = {
    zigbeeModel: ['C6-DOORBELL'],
    model: 'C6-DOORBELL',
    vendor: 'DIY',
    description: 'Sleepy Zigbee doorbell push button (ESP32-C6, deep sleep)',
    fromZigbee: [
        {
            cluster: 'genMultistateInput',
            type: ['attributeReport', 'readResponse'],
            convert: (model, msg) => {
                const value = msg.data['presentValue'];
                if (value === 1) {
                    return {action: 'single'};
                }
                // 0 = button released: nothing to report
                return undefined;
            },
        },
    ],
    toZigbee: [],
    exposes: [exposes.presets.action(['single'])],
};

module.exports = definition;
