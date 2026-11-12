"""
Adds the broker's root CA to a GrainWatch certfile downloaded from Doover, ready to build into the firmware.

Doover's installer package gives a certfile.json with the device's details filled in (DEVICE_NAME, BROKER_ENDPOINT,
CLIENT_ID, USERNAME, PASSWORD, topics, BROKER_PORT, ROOT_CA_URL). The device also needs the CA certificate itself to
check it is talking to the real broker, and cannot download it, so this script fetches it from ROOT_CA_URL and adds it
as ROOT_CA.

The output holds the device's password. Do not commit a filled-in copy.

Run with:  python format_certfile.py <downloaded certfile.json>
       or  drop the downloaded file onto this script, or run it with no argument to pick the file.
"""
import json
import os
import sys
import urllib.request

# Written straight into the folder the firmware embeds it from (main/CMakeLists.txt)
OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main", "config", "certfile.json")

REQUIRED_STRINGS = ("DEVICE_NAME", "BROKER_ENDPOINT", "CLIENT_ID", "USERNAME", "PASSWORD",
                    "MQTT_TOPIC_TO_PUBLISH_TO", "MQTT_TOPIC_TO_SUBSCRIBE_TO", "ROOT_CA_URL")


def pick_input():
    if len(sys.argv) > 1:
        return sys.argv[1]
    try:
        import tkinter
        from tkinter import filedialog
        tkinter.Tk().withdraw()
        path = filedialog.askopenfilename(title="Downloaded certfile.json",
                                          filetypes=[("JSON", "*.json"), ("All files", "*.*")])
        if path:
            return path
    except Exception:
        pass
    return input("Path to the downloaded certfile.json: ").strip().strip('"')


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        text = f.read()
    try:
        return json.loads(text)
    except ValueError:
        # Accept the fields without the surrounding braces, as the template is sometimes shown
        return json.loads("{%s}" % text.strip().rstrip(","))


def check(data):
    """Returns a list of problems that would stop the device connecting. Empty if the file is usable."""
    problems = []
    for key in REQUIRED_STRINGS:
        value = data.get(key)
        if not isinstance(value, str) or not value.strip():
            problems.append("%s is missing or empty" % key)
    for key, value in data.items():
        if isinstance(value, str) and "{{" in value:
            problems.append("%s still has a template placeholder: %s" % (key, value))
    endpoint = data.get("BROKER_ENDPOINT", "")
    if isinstance(endpoint, str) and (":" in endpoint or "/" in endpoint):
        problems.append("BROKER_ENDPOINT must be a bare hostname such as mqtt.doover.com, not %r" % endpoint)
    port = data.get("BROKER_PORT")
    if isinstance(port, str) and port.isdigit():
        data["BROKER_PORT"] = int(port)
    elif not isinstance(port, int) or isinstance(port, bool):
        problems.append("BROKER_PORT must be a number, e.g. 8883")
    return problems


def main():
    path = pick_input()
    if not path:
        print("No file given, nothing written.")
        return 1
    try:
        data = load(path)
    except Exception as e:
        print("Could not read %s as JSON: %s" % (path, e))
        return 1

    problems = check(data)
    if problems:
        print("Not usable, nothing written:")
        for p in problems:
            print("  - " + p)
        return 1

    print("Downloading CA bundle from %s..." % data["ROOT_CA_URL"])
    try:
        with urllib.request.urlopen(data["ROOT_CA_URL"], timeout=30) as response:
            root_ca = response.read().decode("utf-8")
    except Exception as e:
        print("Could not download the root CA, nothing written: %s" % e)
        return 1
    if "-----BEGIN CERTIFICATE-----" not in root_ca:
        print("ROOT_CA_URL did not return a PEM certificate, nothing written.")
        return 1
    data["ROOT_CA"] = root_ca

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=4)
        f.write("\n")

    print("Wrote %s for %s" % (OUTPUT_FILE, data["DEVICE_NAME"]))
    print("  Broker:    %s:%d as %s" % (data["BROKER_ENDPOINT"], data["BROKER_PORT"], data["USERNAME"]))
    print("  Publish:   %s" % data["MQTT_TOPIC_TO_PUBLISH_TO"])
    print("  Subscribe: %s" % data["MQTT_TOPIC_TO_SUBSCRIBE_TO"])
    return 0


if __name__ == "__main__":
    code = main()
    if len(sys.argv) <= 1:
        input("Press Enter to close")  # Keeps the window open when double clicked
    sys.exit(code)
