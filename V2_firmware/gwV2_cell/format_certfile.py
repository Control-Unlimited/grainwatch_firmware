import re
import json
import ssl
import socket
from urllib.parse import urlparse
import urllib.request

def sanitize_json_string(text):

    # Remove trailing comma
    text = re.sub(r',\s*([\]}])', r'\1', text)

    # fix new lines
    result = []
    inside_quotes = False
    
    for char in text:
        # Toggle state if we hit a double quote
        if char == '"':
            inside_quotes = not inside_quotes
            result.append(char)
        # If it's a newline and we are inside quotes, swap it
        elif char == '\n' and inside_quotes:
            result.append('\\n')
        else:
            result.append(char)
            
    return "".join(result)

def fix_and_update_json(filepath):
    try:
        with open(filepath, "r") as f:
            content_raw = f.read()

        content = sanitize_json_string(content_raw)

        data = json.loads(content)
    except Exception as e:
        print(f"Failed to sanitize/parse JSON: {e}")
        return

    # Update Device ID
    device_id = input("Enter the DEVICE_ID: ")
    data["DEVICE_ID"] = device_id

    # Get root cert
    url = data.get("ROOT_CA_URL")
    if url:
        try:
            print(f"Downloading Root CA from {url}...")
            with urllib.request.urlopen(url) as response:
                cert_pem = response.read().decode('utf-8')
            
            data["ROOT_CA"] = cert_pem
        except Exception as e:
            print(f"Could not download cert file: {e}")

    with open(filepath, "w") as f:
        json.dump(data, f, indent=4)

    print(f"Successfully updated {filepath}.")


fix_and_update_json("certfile.json")