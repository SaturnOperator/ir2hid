# ir2hid

This Flipper Zero application lets you control your computer using any standard IR remote. It works by converting IR signals to HID signals using a csv look up table.

### Building

To build from source install UBFT and run

```sh
ufbt build
```

### Configuring

Edit `lut.csv` to configure your remote's IR codes, see sample below:

| ir_protocol | ir_address | ir_command | hid_command | ir_key_comment | hid_key_comment     |
| ----------- | ---------- | ---------- | ----------- | -------------- | ------------------- |
| NECext      | 0x7F00     | 0xA758     | 0x80        | remote vol+    | KEY_MEDIA_VOLUME_UP |

To get the `ir_protocol`, `ir_address`, & `ir_command` you can either use this app or the official Flipper Zero app to get the IR command of each button by pointing and clicking the remote buttons then reading the screen to get the values. 

The `hid_command` value can be obtained from the [USB HID spec](https://usb.org/sites/default/files/hut1_3_0.pdf) section 10.

Columns `ir_key_comment`, &  `hid_key_comment` don't serve any purpose other than being comments to make the LUT more human readable.

### Installation 

1. Upload `ir2hid.fap` as an Infrared application under: `/apps/Infrared/ir2hid.fap`
2. Upload `lut.csv` under `/apps_data/ir2hid/lut.csv`