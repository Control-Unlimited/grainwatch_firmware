# JD608 - Grain Monitor Spear (ESP-IDF)

Created on the esp32-s2-solo-2

## `format_certfile.py`

This is a python regex script that will return a properly formatted .json file from what is given by the website.

### Steps for use

1) Move the cert file into the same directory as the `format_certfile.py` script
2) Rename the cert file to `certfile.json`
3) Run the python script either by double clicking the `format_certfile.py` file or running `python3 format_certfile.py`
4) Move the `certfile.json` back into the `main/config/` folder

## Flow Chart

![Flow chart](JD608_ESP_Prog_Flow.jpg)