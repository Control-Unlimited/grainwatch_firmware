# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "pyserial==3.5",
#     "mpy-cross==1.18",
# ]
# ///
"""
GrainWatch V2 provisioning tool.

Pick Cellular or Wi-Fi, type the device's serial (its Doover display name, e.g. "GW 2 1026 1133") and press
Provision. The tool:

  1. Looks the device up on Doover. A new device is created in A. GRAINWATCH TESTING with the GrainWatch V2 solution
     and the cellular or Wi-Fi device type. If it already exists you choose: do nothing, close, or pull its certfile
     and provision with that.
  2. Downloads the device's certfile from Doover (each download adds a new MQTT key; older keys keep working).
  3. Adds the broker's CA certificate (ROOT_CA), using the same checks as format_certfile.py.
  4. Flashes the device on the chosen COM port:
       Cellular: compiles the code in gwV2_cell/Code with mpy-cross 1.18 (same settings as build_mpy.py) and copies it
                 plus cert/certfile.json onto the XBee through its MicroPython REPL, then resets it.
       Wi-Fi:    puts the certfile in main/config/certfile.json, runs idf.py build flash, then puts the template back.

Open it by double clicking "Provision Device.bat" next to this file (or: uv run provision_device.py). Any missing
package is installed on first run.
Doover login: uses the Doover CLI's saved login. If there isn't one, the tool offers to open `doover login`.
DOOVER_PROFILE picks a profile other than "default".

The certfile holds the device's MQTT password. It is only kept in a temporary folder, and the Wi-Fi template is
restored after every build. The ESP-IDF build folder (gwV2_wifi/build, ignored by git) does hold the last device's
certfile until the next build.
"""
import binascii
import importlib.util
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import traceback
import urllib.error
import urllib.parse
import urllib.request
from tkinter import messagebox, scrolledtext, ttk

# Packages this tool needs beyond the standard library, installed automatically on first run
REQUIRED_PACKAGES = {"serial": "pyserial==3.5", "mpy_cross": "mpy-cross==1.18"}
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def ensure_packages():
    """Installs any missing package with pip, showing a small window while it runs. Exits if that fails."""
    missing = [pkg for module, pkg in REQUIRED_PACKAGES.items() if importlib.util.find_spec(module) is None]
    if not missing:
        return
    python = sys.executable
    if python.lower().endswith("pythonw.exe") and os.path.exists(python[:-5] + ".exe"):
        python = python[:-5] + ".exe"  # pip behaves better under python.exe; the window stays hidden either way
    command = [python, "-m", "pip", "install", "--disable-pip-version-check"]
    if sys.prefix == sys.base_prefix:  # --user is refused inside a virtual environment
        command.append("--user")

    root = tk.Tk()
    root.title("GrainWatch V2 Provisioning")
    ttk.Label(root, text="Installing %s (first run only)..." % ", ".join(missing), padding=24).pack()
    root.update()
    result = subprocess.run(command + missing, capture_output=True, text=True, creationflags=NO_WINDOW)
    if result.returncode != 0 and "No module named pip" in (result.stderr or "") and shutil.which("uv"):
        # Environments made by uv have no pip; uv can install into them directly
        result = subprocess.run(["uv", "pip", "install", "--python", sys.executable] + missing,
                                capture_output=True, text=True, creationflags=NO_WINDOW)
    root.destroy()
    if result.returncode != 0:
        messagebox.showerror("GrainWatch V2 Provisioning", "Couldn't install %s:\n\n%s\n\nInstall them by hand with:"
                             "\n  python -m pip install %s" % (", ".join(missing),
                                                               (result.stderr or result.stdout)[-800:],
                                                               " ".join(missing)))
        sys.exit(1)
    import site
    importlib.invalidate_caches()
    user_site = site.getusersitepackages()
    if user_site not in sys.path:  # A brand-new user site folder isn't on sys.path until Python restarts
        sys.path.append(user_site)


ensure_packages()
import serial  # noqa: E402  (after ensure_packages so a first run can install it)
from serial.tools import list_ports  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WIFI_DIR = os.path.join(HERE, "gwV2_wifi")
CELL_DIR = os.path.join(HERE, "gwV2_cell")
WIFI_CERTFILE = os.path.join(WIFI_DIR, "main", "config", "certfile.json")

# ---------------------------------------------------------------------------------------------------------------------
# Doover setup
# ---------------------------------------------------------------------------------------------------------------------
ORGANISATION_ID = "121780874034293259"  # Control Unlimited
GROUP_ID = "169936668311178253"  # A. GRAINWATCH TESTING
GROUP_NAME = "A. GRAINWATCH TESTING"
SOLUTION_ID = "210896957642684678"  # GrainWatch V2
SOLUTION_NAME = "GrainWatch V2"
DEVICE_TYPES = {
    "cell": ("154048065399975943", "GrainWatch"),
    "wifi": ("210268932660007169", "GrainWatch (Wifi)"),
}
SERIAL_PATTERN = re.compile(r"^GW \d+ \d{4} \d{4}$")

# ---------------------------------------------------------------------------------------------------------------------
# USB IDs used to suggest a COM port
# ---------------------------------------------------------------------------------------------------------------------
VID_ESPRESSIF = 0x303A  # ESP32-S2 native USB
VID_FTDI = 0x0403  # Digi XBee development boards and many USB-serial adapters
VID_SILABS = 0x10C4  # CP210x adapters
VID_WCH = 0x1A86  # CH340 adapters

REPL_BAUDS = (115200, 9600, 230400, 57600, 38400, 19200)  # Order tried when looking for the XBee's REPL
CHUNK_BYTES = 192  # Bytes per REPL write line when copying files to the XBee


class ProvisionError(Exception):
    """A step failed in a way the operator needs to see. The message is shown as is."""


class LoginRequired(ProvisionError):
    """No usable Doover CLI login on this PC."""


class Cancelled(Exception):
    """The operator chose to stop."""


# ---------------------------------------------------------------------------------------------------------------------
# Doover API
# ---------------------------------------------------------------------------------------------------------------------
class Doover:
    """Minimal Doover control API client using the Doover CLI's saved login (~/.doover/config)."""

    def __init__(self, profile="default"):
        path = os.path.expanduser("~/.doover/config")
        try:
            text = open(path, encoding="utf-8").read()
        except OSError:
            raise LoginRequired("This PC isn't logged in to Doover.")
        marker = "[profile=%s]" % profile
        if marker not in text:
            raise LoginRequired("No Doover login called '%s' on this PC." % profile)
        block = text.split(marker, 1)[1].split("[profile=", 1)[0]
        self.cfg = dict(line.split("=", 1) for line in block.strip().splitlines() if "=" in line)
        self.base = (self.cfg.get("BASE_URL") or "https://api.doover.com").rstrip("/")
        self.token = None
        self.token_time = 0

    def _refresh(self):
        params = urllib.parse.urlencode({
            "grant_type": "refresh_token",
            "access_token": self.cfg.get("TOKEN", ""),
            "refresh_token": self.cfg.get("REFRESH_TOKEN", ""),
            "client_id": self.cfg.get("AUTH_SERVER_CLIENT_ID", ""),
        })
        url = self.cfg.get("AUTH_SERVER_URL", "https://auth.doover.com").rstrip("/") + "/oauth2/token?" + params
        try:
            with urllib.request.urlopen(urllib.request.Request(url, data=b"", method="POST"), timeout=30) as r:
                self.token = json.load(r)["access_token"]
        except urllib.error.HTTPError as e:
            raise LoginRequired("The Doover login on this PC has expired (HTTP %s)." % e.code)
        self.token_time = time.time()

    def request(self, method, path, body=None, raw=False):
        if not self.token or time.time() - self.token_time > 1800:
            self._refresh()
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method, headers={
            "Authorization": "Bearer " + self.token,
            "X-Doover-Organisation": ORGANISATION_ID,
            "Content-Type": "application/json",
            "Accept": "application/json",
        })
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                content = r.read()
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:400]
            raise ProvisionError("Doover %s %s failed: HTTP %s %s" % (method, path, e.code, detail))
        except urllib.error.URLError as e:
            raise ProvisionError("Could not reach Doover: %s" % e.reason)
        return content if raw else json.loads(content or b"null")

    def find_device(self, display_name):
        """Returns the device whose display name matches (ignoring case and extra spaces), or None."""
        wanted = normalise_serial(display_name).lower()
        query = urllib.parse.urlencode({"search": wanted, "per_page": 100})
        page = self.request("GET", "/devices/?" + query)
        for device in page.get("results", []):
            if normalise_serial(device.get("display_name", "")).lower() == wanted:
                return device
        return None

    def create_device(self, display_name, kind):
        type_id = DEVICE_TYPES[kind][0]
        device = self.request("POST", "/devices/", {
            "display_name": display_name,
            "name": display_name.lower().replace(" ", "-"),
            "group_id": GROUP_ID,
            "type_id": type_id,
            "solution_id": SOLUTION_ID,
        })
        return device

    def ensure_solution(self, device_id, log):
        installs = self.request("GET", "/devices/%s/solution_installs/" % device_id).get("results", [])
        if any(str((i.get("solution") or {}).get("id", i.get("solution"))) == SOLUTION_ID or
               i.get("display_name") == SOLUTION_NAME for i in installs):
            log("%s is installed" % SOLUTION_NAME)
            return
        log("Installing %s..." % SOLUTION_NAME)
        self.request("POST", "/solution_installs/", {
            "solution_id": SOLUTION_ID,
            "device_id": device_id,
            "display_name": SOLUTION_NAME,
            "deployment_config": {},
        })
        log("%s installed" % SOLUTION_NAME)

    def download_certfile(self, device_id):
        return self.request("GET", "/devices/%s/installer/download/" % device_id, raw=True)


def normalise_serial(text):
    return " ".join(text.split())


# ---------------------------------------------------------------------------------------------------------------------
# Certfile
# ---------------------------------------------------------------------------------------------------------------------
def load_format_certfile():
    """Imports gwV2_wifi/format_certfile.py so the certfile checks stay in one place."""
    path = os.path.join(WIFI_DIR, "format_certfile.py")
    spec = importlib.util.spec_from_file_location("format_certfile", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_certfile(raw_bytes, expected_name, log):
    """Checks the downloaded certfile, adds ROOT_CA and returns it as a dict."""
    fmt = load_format_certfile()
    text = raw_bytes.decode("utf-8-sig")
    data = None
    # strict=False accepts raw newlines inside strings, which older templates put in their PEM fields
    for candidate in (text, "{%s}" % text.strip().rstrip(",")):
        try:
            data = json.loads(candidate, strict=False)
            break
        except ValueError:
            pass
    if not isinstance(data, dict):
        raise ProvisionError("The file Doover returned isn't a JSON certfile. Check the device's type uses the "
                             "GrainWatch V2 install template.\n\nStart of the file:\n%s" % text[:200])

    problems = fmt.check(data)
    if problems:
        hint = ""
        if "PASSWORD" not in data:
            hint = "\n\nThis device's type still uses the old AWS certfile. Change it to a GrainWatch V2 type first."
        raise ProvisionError("The certfile from Doover is not usable:\n  - " + "\n  - ".join(problems) + hint)
    if normalise_serial(data["DEVICE_NAME"]).lower() != normalise_serial(expected_name).lower():
        raise ProvisionError("The certfile is for %r, not %r." % (data["DEVICE_NAME"], expected_name))

    log("Downloading CA bundle from %s" % data["ROOT_CA_URL"])
    try:
        with urllib.request.urlopen(data["ROOT_CA_URL"], timeout=30) as r:
            root_ca = r.read().decode("utf-8")
    except Exception as e:
        raise ProvisionError("Could not download the root CA: %s" % e)
    if "-----BEGIN CERTIFICATE-----" not in root_ca:
        raise ProvisionError("ROOT_CA_URL did not return a PEM certificate.")
    data["ROOT_CA"] = root_ca
    log("Certfile ready: %s as %s, publishing to %s" % (data["BROKER_ENDPOINT"], data["USERNAME"],
                                                        data["MQTT_TOPIC_TO_PUBLISH_TO"]))
    return data


def certfile_text(data):
    return json.dumps(data, indent=4) + "\n"


# ---------------------------------------------------------------------------------------------------------------------
# COM ports
# ---------------------------------------------------------------------------------------------------------------------
def list_serial_ports(kind):
    """Returns [(device, label)] with the most likely port for this kind first."""
    def score(p):
        text = " ".join(filter(None, [p.description, p.manufacturer, p.product])).lower()
        if kind == "cell":
            if "xbee" in text or "digi" in text:
                return 0
            if p.vid == VID_FTDI:
                return 1
            if p.vid in (VID_SILABS, VID_WCH):
                return 2
        else:
            if p.vid == VID_ESPRESSIF or "esp32" in text:
                return 0
            if p.vid in (VID_SILABS, VID_WCH):
                return 1
            if p.vid == VID_FTDI:
                return 2
        return 3 if p.vid else 4  # Bluetooth and other virtual ports last

    ports = sorted(list_ports.comports(), key=lambda p: (score(p), p.device))
    return [(p.device, "%s - %s" % (p.device, p.description)) for p in ports]


# ---------------------------------------------------------------------------------------------------------------------
# Cellular: XBee 3 over its MicroPython REPL
# ---------------------------------------------------------------------------------------------------------------------
def build_cell_files(log):
    """Compiles gwV2_cell/Code into a temp folder exactly as build_mpy.py does. Returns [(local, /flash path)]."""
    spec = importlib.util.spec_from_file_location("build_mpy", os.path.join(CELL_DIR, "build_mpy.py"))
    build_mpy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_mpy)
    import mpy_cross

    out_dir = tempfile.mkdtemp(prefix="gw_mpy_")
    files = []
    for module in build_mpy.MODULES:
        source = os.path.join(build_mpy.CODE_DIR, module + ".py")
        target = os.path.join(out_dir, module + ".mpy")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        process = mpy_cross.run("-mno-unicode", "-s", "/flash/%s.py" % module, "-o", target, source,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = process.communicate()[0].decode().strip()
        if process.returncode != 0 or output:
            raise ProvisionError("mpy-cross failed on %s.py:\n%s" % (module, output))
        files.append((target, "/flash/%s.mpy" % module))
        log("Compiled %s.mpy (%d bytes)" % (module, os.path.getsize(target)))
    return out_dir, files


def quiet(statement):
    """One REPL line that runs statement and ignores OSError (folder already exists, file missing)."""
    return 'exec("try:\\n %s\\nexcept OSError:\\n pass")' % statement


class XBeeRepl:
    """Talks to the XBee's MicroPython REPL (AP=4) over the serial port."""

    PROMPT = b">>> "

    def __init__(self, port, log):
        self.port = port
        self.log = log
        self.ser = None

    def connect(self, wait_seconds=60, cancelled=lambda: False):
        """Finds the REPL's baud rate and stops any running program. Keeps trying while the module wakes."""
        deadline = time.time() + wait_seconds
        attempt = 0
        while time.time() < deadline:
            if cancelled():
                raise Cancelled()
            for baud in REPL_BAUDS:
                try:
                    self.ser = serial.Serial(self.port, baud, timeout=0.2, write_timeout=5)
                except serial.SerialException as e:
                    raise ProvisionError("Could not open %s: %s" % (self.port, e))
                self.ser.reset_input_buffer()
                self.ser.write(b"\x03\x03\r\n")  # Ctrl-C stops main.py and returns to the prompt
                if self._read_until(self.PROMPT, 1.0).endswith(self.PROMPT):
                    self.log("XBee MicroPython REPL found at %d baud" % baud)
                    return
                self.ser.close()
                self.ser = None
            attempt += 1
            if attempt == 1:
                self.log("No REPL yet. If the spear is asleep, power-cycle it or press reset on the dev board...")
        raise ProvisionError("No MicroPython prompt on %s.\n\nCheck the XBee is on the dev board, powered, in "
                             "MicroPython REPL mode (AP=4), and that this is the right COM port. If it was asleep, "
                             "power-cycle it and try again." % self.port)

    def _read_until(self, marker, timeout):
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk
                if buf.endswith(marker):
                    break
        return buf

    def run(self, line, timeout=10.0):
        """Runs one line at the prompt and returns its output. Raises on a Python error."""
        self.ser.write(line.encode() + b"\r")
        out = self._read_until(self.PROMPT, timeout)
        if not out.endswith(self.PROMPT):
            raise ProvisionError("XBee stopped responding while running: %s" % line[:60])
        text = out[:-len(self.PROMPT)].decode("utf-8", "replace")
        # Drop the echoed command
        text = text.split("\r\n", 1)[1] if "\r\n" in text else ""
        if "Traceback" in text:
            raise ProvisionError("XBee error running %s:\n%s" % (line[:60], text.strip()))
        return text.strip()

    def put_file(self, data, remote):
        folder = remote.rsplit("/", 1)[0]
        parts = folder.strip("/").split("/")
        for i in range(2, len(parts) + 1):  # /flash already exists
            self.run(quiet("os.mkdir('/%s')" % "/".join(parts[:i])))
        self.run("f = open('%s', 'wb')" % remote)
        for i in range(0, len(data), CHUNK_BYTES):
            self.run("w(u('%s'))" % binascii.hexlify(data[i:i + CHUNK_BYTES]).decode())
        self.run("f.close()")
        size = self.run("print(os.stat('%s')[6])" % remote)
        if size != str(len(data)):
            raise ProvisionError("%s is %s bytes on the XBee, expected %d" % (remote, size, len(data)))

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None


def flash_cell(port, certfile, log, cancelled):
    out_dir, files = build_cell_files(log)
    repl = XBeeRepl(port, log)
    try:
        repl.connect(cancelled=cancelled)
        repl.run("import os, xbee, ubinascii")
        repl.run("u = ubinascii.unhexlify")
        repl.run("w = lambda b: f.write(b)")
        # Stale source copies would run instead of, or alongside, the compiled files
        for _, remote in files:
            repl.run(quiet("os.remove('%s.py')" % remote[:-len(".mpy")]))
        uploads = [(open(local, "rb").read(), remote) for local, remote in files]
        uploads.append((certfile_text(certfile).encode(), "/flash/cert/certfile.json"))
        total = sum(len(d) for d, _ in uploads)
        sent = 0
        for data, remote in uploads:
            if cancelled():
                raise Cancelled()
            log("Copying %s (%d bytes)" % (remote, len(data)))
            repl.put_file(data, remote)
            sent += len(data)
            log("  %d%% copied" % (100 * sent // total))
        repl.run("xbee.atcmd('PS', 1)")  # Run main at power-up
        repl.run("xbee.atcmd('WR')")
        log("Files copied. Resetting the XBee...")
        repl.ser.write(b"xbee.atcmd('FR')\r")  # Resets the module, so there is no prompt to wait for
        time.sleep(1)
    finally:
        repl.close()
        shutil.rmtree(out_dir, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------------------------
# Wi-Fi: ESP-IDF build and flash
# ---------------------------------------------------------------------------------------------------------------------
def find_idf():
    """Returns (idf_path, python) for the ESP-IDF install, from IDF_PATH or the ESP-IDF installer's manifest."""
    manifest = os.path.expanduser("~/.espressif/eim_idf.json")
    if os.path.exists(manifest):
        info = json.load(open(manifest, encoding="utf-8"))
        installs = info.get("idfInstalled", [])
        chosen = next((i for i in installs if i.get("id") == info.get("idfSelectedId")), installs[0] if installs else None)
        if chosen and os.path.isdir(chosen.get("path", "")):
            return chosen["path"], chosen.get("python") or sys.executable
    idf_path = os.environ.get("IDF_PATH")
    if idf_path and os.path.isdir(idf_path):
        env_python = os.environ.get("IDF_PYTHON_ENV_PATH")
        python = os.path.join(env_python, "Scripts", "python.exe") if env_python else sys.executable
        return idf_path, python
    raise ProvisionError("ESP-IDF not found. Install ESP-IDF v5.5 (the Wi-Fi firmware's version) or set IDF_PATH.")


def idf_environment(idf_path, python):
    env = dict(os.environ)
    env.pop("MSYSTEM", None)  # idf_tools.py refuses to run when it thinks it is inside Git Bash / MSYS
    env["IDF_PATH"] = idf_path
    result = subprocess.run([python, os.path.join(idf_path, "tools", "idf_tools.py"), "export", "--format",
                             "key-value"], capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise ProvisionError("Could not set up the ESP-IDF environment:\n%s" % (result.stderr or result.stdout))
    for line in result.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.upper() == "PATH":
            value = value.replace("%PATH%", env.get("PATH", ""))
            if env.get("PATH", "") not in value:
                value = value + os.pathsep + env.get("PATH", "")
        env[key] = value
    return env


BUILD_STEP = re.compile(r"^\[(\d+)/(\d+)\]")  # ninja: [123/1500] Building C object ...
# esptool 4: "Writing at 0x00010000... (42 %)"   esptool 5: "Writing at 0x00010000 [====>    ]  42.1% ..."
WRITE_PROGRESS = re.compile(r"Writing at 0x[0-9a-f]+\D*?(\d+)(?:\.\d+)? ?%")


def clear_moved_build_dir(log):
    """
    Deletes gwV2_wifi/build if any of its CMake caches was made in another folder (e.g. copied from an older project).
    CMake refuses to build with such a cache. The folder is generated and ignored by git, so it is safe to remove.
    """
    build_dir = os.path.join(WIFI_DIR, "build")
    reason = moved_build_dir_reason(build_dir)
    if reason:
        log("%s. Clearing gwV2_wifi/build for a clean build (takes a few minutes, once)." % reason)
        shutil.rmtree(build_dir)


def moved_build_dir_reason(build_dir):
    """Why build_dir belongs to another folder, or None if it is fine (or doesn't exist yet)."""
    here = WIFI_DIR.replace("\\", "/").lower()
    for sub in ("", "bootloader"):
        folder = os.path.join(build_dir, sub)
        cache = os.path.join(folder, "CMakeCache.txt")
        if os.path.exists(cache):
            with open(cache, encoding="utf-8", errors="replace") as f:
                made_in = next((line.split("=", 1)[1].strip() for line in f
                                if line.startswith("CMAKE_CACHEFILE_DIR:")), None)
            if made_in and os.path.normcase(os.path.normpath(made_in)) != os.path.normcase(os.path.normpath(folder)):
                return "The build folder was made in %s" % made_in
        # A failed configure can rewrite the cache but leave build.ninja pointing at the old folder
        ninja = os.path.join(folder, "build.ninja")
        if os.path.exists(ninja):
            with open(ninja, encoding="utf-8", errors="replace") as f:
                if here not in f.read().replace("\\", "/").lower():
                    return "The build folder was made for another copy of this project"
    return None


def flash_wifi(port, certfile, log, cancelled, progress=lambda text: None):
    idf_path, python = find_idf()
    log("ESP-IDF: %s" % idf_path)
    env = idf_environment(idf_path, python)
    clear_moved_build_dir(log)

    template = open(WIFI_CERTFILE, "rb").read()
    try:
        with open(WIFI_CERTFILE, "w", encoding="utf-8", newline="\n") as f:
            f.write(certfile_text(certfile))
        command = [python, os.path.join(idf_path, "tools", "idf.py"), "-p", port, "build", "flash"]
        log("Running idf.py -p %s build flash (the first build can take a few minutes)" % port)
        process = subprocess.Popen(command, cwd=WIFI_DIR, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace",
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        tail = []
        for line in process.stdout:
            line = line.rstrip()
            tail = (tail + [line])[-40:]
            if line:
                log("  " + line, verbose=True)
            built = BUILD_STEP.match(line)
            written = WRITE_PROGRESS.search(line)
            if built:
                done, total = int(built.group(1)), int(built.group(2))
                progress("Building firmware... %d%% (%d of %d)" % (100 * done // total, done, total))
            elif "Connecting..." in line or line.startswith("esptool"):
                progress("Connecting to the ESP32-S2 on %s..." % port)
            elif written:
                progress("Writing firmware... %s%%" % written.group(1))
            if cancelled():
                process.kill()
                raise Cancelled()
        if process.wait() != 0:
            raise ProvisionError("idf.py build flash failed. Last output:\n" + "\n".join(tail[-15:]) +
                                 "\n\nIf it could not connect, hold BOOT, tap RESET, release BOOT and try again.")
    finally:
        with open(WIFI_CERTFILE, "wb") as f:  # Never leave a device's password in the tracked template
            f.write(template)
        log("Restored the certfile template in main/config")


# ---------------------------------------------------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------------------------------------------------
STEPS = ["Find device", "Create", "Certfile", "Root CA", "Flash"]
STEP_DETAIL = ["Find device on Doover", "Create device", "Download certfile", "Add root CA", "Flash device"]

# Palette
BG = "#f3f5f4"
CARD = "#ffffff"
BORDER = "#d9dedb"
TEXT = "#1d2421"
MUTED = "#66706b"
PRIMARY = "#1f6f50"  # GrainWatch green
PRIMARY_HOVER = "#185a41"
PRIMARY_TINT = "#e6f2ec"
DISABLED = "#b8c2bd"
STATE_COLOURS = {  # (accent, tint) for the status banner and stepper
    "idle": ("#8a948f", "#eef1ef"),
    "run": ("#1f6feb", "#e8f0fe"),
    "ok": ("#1a7f37", "#e6f4ea"),
    "warn": ("#9a6700", "#fff5d6"),
    "fail": ("#cf222e", "#fdecee"),
    "skip": ("#8a948f", "#eef1ef"),
    "todo": ("#c3cbc7", CARD),
}
FONT = "Segoe UI"
ICON_FONT = "Segoe MDL2 Assets"  # Built into Windows 10 and 11
ICON_CELL = ""  # Cellular bars
ICON_WIFI = ""  # Wi-Fi


class FlatButton(tk.Label):
    """A flat, coloured button with a hover state (ttk buttons can't be coloured with the native theme)."""

    def __init__(self, parent, text, command, primary=True, **kw):
        self.colours = (PRIMARY, PRIMARY_HOVER, "#ffffff") if primary else (CARD, "#eef1ef", TEXT)
        super().__init__(parent, text=text, cursor="hand2", padx=22 if primary else 16, pady=9 if primary else 7,
                         font=(FONT, 11, "bold") if primary else (FONT, 10), bg=self.colours[0], fg=self.colours[2],
                         highlightthickness=0 if primary else 1, highlightbackground=BORDER, **kw)
        self.command = command
        self.enabled = True
        self.bind("<Enter>", lambda e: self.enabled and self.config(bg=self.colours[1]))
        self.bind("<Leave>", lambda e: self.enabled and self.config(bg=self.colours[0]))
        self.bind("<Button-1>", lambda e: self.enabled and self.command())

    def set_enabled(self, enabled):
        self.enabled = enabled
        if enabled:
            self.config(bg=self.colours[0], fg=self.colours[2], cursor="hand2")
        else:
            self.config(bg=DISABLED if self.colours[0] == PRIMARY else CARD,
                        fg="#ffffff" if self.colours[0] == PRIMARY else DISABLED, cursor="arrow")


class DeviceCard(tk.Frame):
    """One of the two Cellular / Wi-Fi choices. Click anywhere on it to select it."""

    def __init__(self, parent, value, variable, icon, title, subtitle, on_change, has_icons):
        super().__init__(parent, bg=CARD, highlightthickness=2, highlightbackground=BORDER, cursor="hand2")
        self.value, self.variable, self.on_change = value, variable, on_change
        inner = tk.Frame(self, bg=CARD, padx=14, pady=10)
        inner.pack(fill="both", expand=True)
        self.parts = [self, inner]
        if has_icons:
            glyph = tk.Label(inner, text=icon, font=(ICON_FONT, 20), bg=CARD, fg=PRIMARY)
            glyph.pack(side="left", padx=(0, 12))
            self.parts.append(glyph)
            self.glyph = glyph
        else:
            self.glyph = None
        words = tk.Frame(inner, bg=CARD)
        words.pack(side="left", fill="x")
        self.title = tk.Label(words, text=title, font=(FONT, 11, "bold"), bg=CARD, fg=TEXT, anchor="w")
        self.title.pack(anchor="w")
        self.subtitle = tk.Label(words, text=subtitle, font=(FONT, 9), bg=CARD, fg=MUTED, anchor="w")
        self.subtitle.pack(anchor="w")
        self.parts += [words, self.title, self.subtitle]
        for widget in self.parts:
            widget.bind("<Button-1>", self.select)
        variable.trace_add("write", lambda *a: self.refresh())
        self.refresh()

    def select(self, _event=None):
        if str(self.cget("cursor")) == "arrow":  # Locked while provisioning
            return
        self.variable.set(self.value)
        self.on_change()

    def refresh(self):
        selected = self.variable.get() == self.value
        bg = PRIMARY_TINT if selected else CARD
        self.config(highlightbackground=PRIMARY if selected else BORDER)
        for widget in self.parts[1:]:
            widget.config(bg=bg)

    def set_enabled(self, enabled):
        for widget in self.parts:
            widget.config(cursor="hand2" if enabled else "arrow")


class Stepper(tk.Canvas):
    """Horizontal progress: a numbered circle per step joined by a line, with the step name underneath."""

    def __init__(self, parent):
        super().__init__(parent, height=70, bg=CARD, highlightthickness=0)
        self.states = ["todo"] * len(STEPS)
        self.bind("<Configure>", lambda e: self.draw())

    def set(self, index, state):
        self.states[index] = state
        self.draw()

    def reset(self):
        self.states = ["todo"] * len(STEPS)
        self.draw()

    def draw(self):
        self.delete("all")
        width = max(self.winfo_width(), 200)
        count = len(STEPS)
        margin = 44
        gap = (width - 2 * margin) / (count - 1)
        y, r = 22, 13
        for i in range(count - 1):
            done = self.states[i] in ("ok", "skip")
            self.create_line(margin + i * gap + r + 4, y, margin + (i + 1) * gap - r - 4, y, width=2,
                             fill=STATE_COLOURS["ok"][0] if done else BORDER)
        for i, (name, state) in enumerate(zip(STEPS, self.states)):
            x = margin + i * gap
            accent = STATE_COLOURS[state][0]
            if state == "todo":
                self.create_oval(x - r, y - r, x + r, y + r, outline=accent, width=2, fill=CARD)
                self.create_text(x, y, text=str(i + 1), fill=MUTED, font=(FONT, 9, "bold"))
            else:
                symbol = {"ok": "✓", "fail": "✕", "skip": "–", "run": str(i + 1)}[state]
                if state == "run":
                    halo = r + 4
                    self.create_oval(x - halo, y - halo, x + halo, y + halo, outline="", fill=STATE_COLOURS["run"][1])
                self.create_oval(x - r, y - r, x + r, y + r, outline=accent, width=2, fill=accent)
                self.create_text(x, y, text=symbol, fill="#ffffff", font=(FONT, 10, "bold"))
            self.create_text(x, y + r + 14, text=name, font=(FONT, 9, "bold" if state == "run" else "normal"),
                             fill=TEXT if state in ("run", "ok", "fail") else MUTED)


class App:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.worker = None
        self.cancel_flag = threading.Event()
        self.details_open = False
        has_icons = ICON_FONT in tkfont.families()

        root.title("GrainWatch V2 Provisioning")
        root.configure(bg=BG)
        root.minsize(680, 540)
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TEntry", fieldbackground=CARD, bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER,
                        padding=(10, 7))
        style.map("TEntry", bordercolor=[("focus", PRIMARY)], lightcolor=[("focus", PRIMARY)])
        style.configure("TCombobox", fieldbackground=CARD, background=CARD, bordercolor=BORDER, lightcolor=BORDER,
                        darkcolor=BORDER, arrowcolor=MUTED, padding=(8, 5))
        style.map("TCombobox", fieldbackground=[("readonly", CARD)], selectbackground=[("readonly", CARD)],
                  selectforeground=[("readonly", TEXT)], bordercolor=[("focus", PRIMARY)])
        style.configure("Card.TCheckbutton", background=CARD, foreground=MUTED, font=(FONT, 9))
        style.map("Card.TCheckbutton", background=[("active", CARD)])
        root.option_add("*TCombobox*Listbox.font", (FONT, 10))

        # Header band
        header = tk.Frame(root, bg=PRIMARY, padx=24, pady=16)
        header.pack(fill="x")
        tk.Label(header, text="GrainWatch V2 Provisioning", font=(FONT, 16, "bold"), bg=PRIMARY,
                 fg="#ffffff").pack(anchor="w")
        tk.Label(header, text="Set up a spear on Doover and flash it, in one go", font=(FONT, 10), bg=PRIMARY,
                 fg="#cfe6db").pack(anchor="w")

        body = tk.Frame(root, bg=BG, padx=24, pady=18)
        body.pack(fill="both", expand=True)

        # Device type
        self.section(body, "1  Device type")
        self.kind = tk.StringVar(value="cell")
        cards = tk.Frame(body, bg=BG)
        cards.pack(fill="x", pady=(0, 14))
        cards.columnconfigure((0, 1), weight=1, uniform="cards")
        self.cards = [
            DeviceCard(cards, "cell", self.kind, ICON_CELL, "Cellular", "XBee 3 LTE-M", self.refresh_ports,
                       has_icons),
            DeviceCard(cards, "wifi", self.kind, ICON_WIFI, "Wi-Fi", "ESP32-S2", self.refresh_ports, has_icons),
        ]
        self.cards[0].grid(row=0, column=0, sticky="ew", padx=(0, 6))
        self.cards[1].grid(row=0, column=1, sticky="ew", padx=(6, 0))

        # Serial and port
        fields = tk.Frame(body, bg=BG)
        fields.pack(fill="x")
        fields.columnconfigure(0, weight=3, uniform="f")
        fields.columnconfigure(1, weight=2, uniform="f")

        left = tk.Frame(fields, bg=BG)
        left.grid(row=0, column=0, sticky="ew", padx=(0, 6))
        self.section(left, "2  Serial number")
        self.serial = tk.StringVar()
        self.entry = ttk.Entry(left, textvariable=self.serial, font=(FONT, 12))
        self.entry.pack(fill="x")
        self.entry.focus_set()
        self.entry.bind("<Return>", lambda e: self.start())
        tk.Label(left, text="As on Doover, e.g. GW 2 1026 1133", font=(FONT, 9), bg=BG, fg=MUTED).pack(
            anchor="w", pady=(3, 0))

        right = tk.Frame(fields, bg=BG)
        right.grid(row=0, column=1, sticky="ew", padx=(6, 0))
        self.section(right, "3  COM port")
        port_row = tk.Frame(right, bg=BG)
        port_row.pack(fill="x")
        self.port_choice = tk.StringVar()
        self.port_box = ttk.Combobox(port_row, textvariable=self.port_choice, state="readonly", font=(FONT, 10),
                                     width=22)
        self.port_box.pack(side="left", fill="x", expand=True)
        refresh = tk.Label(port_row, text="↻", font=(FONT, 13), bg=BG, fg=PRIMARY, cursor="hand2", padx=6)
        refresh.pack(side="left")
        refresh.bind("<Button-1>", lambda e: self.refresh_ports())
        self.port_hint = tk.Label(right, text="", font=(FONT, 9), bg=BG, fg=MUTED, anchor="w", justify="left")
        self.port_hint.pack(anchor="w", pady=(3, 0))

        # Actions
        actions = tk.Frame(body, bg=BG)
        actions.pack(fill="x", pady=(16, 14))
        self.go = FlatButton(actions, "Provision", self.start)
        self.go.pack(side="left")
        self.stop = FlatButton(actions, "Cancel", self.cancel, primary=False)
        self.stop.pack(side="left", padx=10)
        self.stop.set_enabled(False)

        # Progress card: stepper, status banner, collapsible details
        card = tk.Frame(body, bg=CARD, highlightthickness=1, highlightbackground=BORDER, padx=16, pady=12)
        card.pack(fill="both", expand=True)
        self.stepper = Stepper(card)
        self.stepper.pack(fill="x")

        self.banner = tk.Frame(card, bg=STATE_COLOURS["idle"][1])
        self.banner.pack(fill="x", pady=(6, 0))
        self.banner_bar = tk.Frame(self.banner, bg=STATE_COLOURS["idle"][0], width=4)
        self.banner_bar.pack(side="left", fill="y")
        self.status = tk.Label(self.banner, text="Ready", font=(FONT, 10, "bold"), bg=STATE_COLOURS["idle"][1],
                               fg=TEXT, anchor="w", justify="left", padx=10, pady=8, wraplength=560)
        self.status.pack(side="left", fill="x", expand=True)

        toggles = tk.Frame(card, bg=CARD)
        toggles.pack(fill="x", pady=(8, 0))
        self.details_link = tk.Label(toggles, text="▸ Show details", font=(FONT, 9), bg=CARD, fg=PRIMARY,
                                     cursor="hand2")
        self.details_link.pack(side="left")
        self.details_link.bind("<Button-1>", lambda e: self.toggle_details())

        self.log_box = scrolledtext.ScrolledText(card, height=10, font=("Consolas", 9), state="disabled",
                                                 wrap="word", bg="#f7f8f7", fg=TEXT, relief="flat",
                                                 highlightthickness=1, highlightbackground=BORDER, padx=8, pady=6)

        self.toggle_details(True)  # Output always visible; the link can still hide it
        self.refresh_ports()
        root.after(100, self.pump)

    def section(self, parent, text):
        tk.Label(parent, text=text.upper(), font=(FONT, 8, "bold"), bg=BG, fg=MUTED).pack(anchor="w", pady=(0, 5))

    def toggle_details(self, show=None):
        self.details_open = (not self.details_open) if show is None else show
        if self.details_open:
            self.log_box.pack(fill="both", expand=True, pady=(6, 0))
            self.details_link.config(text="▾ Hide details")
        else:
            self.log_box.pack_forget()
            self.details_link.config(text="▸ Show details")

    def set_status(self, text, state="idle"):
        accent, tint = STATE_COLOURS[state]
        self.banner.config(bg=tint)
        self.banner_bar.config(bg=accent)
        self.status.config(text=text, bg=tint, fg=accent if state in ("ok", "fail", "warn") else TEXT)

    def set_busy(self, busy):
        self.go.set_enabled(not busy)
        self.stop.set_enabled(busy)
        for card in self.cards:
            card.set_enabled(not busy)
        self.entry.config(state="disabled" if busy else "normal")
        self.port_box.config(state="disabled" if busy else "readonly")

    # Port list -------------------------------------------------------------------------------------------------------
    def refresh_ports(self):
        kind = self.kind.get()
        ports = list_serial_ports(kind)
        self.ports = ports
        self.port_box["values"] = [label for _, label in ports]
        if ports:
            if self.port_choice.get() not in self.port_box["values"]:
                self.port_box.current(0)
        else:
            self.port_choice.set("")
        if not ports:
            self.port_hint.config(text="No COM ports found. Plug the device in and press ↻.")
        elif kind == "cell":
            self.port_hint.config(text="XBee in MicroPython REPL mode (AP=4)")
        else:
            self.port_hint.config(text="ESP32-S2 USB or USB-serial adapter")

    def selected_port(self):
        label = self.port_choice.get()
        for device, text in self.ports:
            if text == label:
                return device
        return None

    # Worker plumbing -------------------------------------------------------------------------------------------------
    def post(self, *event):
        self.events.put(event)

    def pump(self):
        try:
            while True:
                event = self.events.get_nowait()
                kind = event[0]
                if kind == "log":
                    self._append(event[1])
                elif kind == "step":
                    self.stepper.set(event[1], event[2])
                elif kind == "status":
                    self.set_status(event[1], event[2])
                    if event[2] == "fail":
                        self.toggle_details(True)
                elif kind == "ask":
                    _, fn, box = event
                    box["answer"] = fn()
                    box["done"].set()
                elif kind == "close":
                    self.root.destroy()
                    return
                elif kind == "done":
                    self.set_busy(False)
        except queue.Empty:
            pass
        self.root.after(80, self.pump)

    def _append(self, text):
        self.log_box.config(state="normal")
        self.log_box.insert("end", text + "\n")
        self.log_box.see("end")
        self.log_box.config(state="disabled")

    def ask(self, fn):
        """Runs a dialog on the UI thread from the worker and returns its answer."""
        box = {"done": threading.Event()}
        self.post("ask", fn, box)
        box["done"].wait()
        return box["answer"]

    def log(self, text, verbose=False):
        self.post("log", text)  # Everything is shown, build output included

    # Run -------------------------------------------------------------------------------------------------------------
    def start(self):
        if self.worker and self.worker.is_alive():
            return
        serial_number = normalise_serial(self.serial.get())
        port = self.selected_port()
        if not serial_number:
            messagebox.showwarning("Serial number", "Type the device's serial number.")
            return
        if not SERIAL_PATTERN.match(serial_number):
            if not messagebox.askyesno("Serial number", "%r doesn't look like a GrainWatch serial "
                                       "(GW 2 1026 1133).\n\nUse it anyway?" % serial_number):
                return
        if not port:
            messagebox.showwarning("COM port", "Connect the device and pick its COM port.")
            return
        self.serial.set(serial_number)
        self.cancel_flag.clear()
        self.stepper.reset()
        self.log_box.config(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.config(state="disabled")
        self.set_busy(True)
        self.worker = threading.Thread(target=self.run, args=(self.kind.get(), serial_number, port), daemon=True)
        self.worker.start()

    def cancel(self):
        self.cancel_flag.set()
        self.set_status("Cancelling...", "warn")

    def run(self, kind, serial_number, port):
        current = [0]

        def step(index, state="run"):
            current[0] = index
            self.post("step", index, state)

        def status(text, state="run"):
            self.post("status", text, state)

        try:
            type_id, type_name = DEVICE_TYPES[kind]
            self.log("%s, %s on %s" % (serial_number, type_name, port))

            step(0)
            status("Looking up %s on Doover..." % serial_number)
            doover = Doover(os.environ.get("DOOVER_PROFILE", "default"))
            device = doover.find_device(serial_number)
            step(0, "ok")

            if device:
                status("%s is already on Doover" % serial_number, "warn")
                choice = self.ask(lambda: ExistsDialog(self.root, device, type_id).result)
                if choice == "close":
                    self.post("close")
                    return
                if choice != "provision":
                    raise Cancelled()
                step(1, "skip")
                self.log("Using existing device %s (agent %s)" % (device["display_name"], device["id"]))
            else:
                step(1)
                if not self.ask(lambda: messagebox.askyesno(
                        "Create device", "%s isn't on Doover.\n\nCreate it in %s as %s with the %s solution?"
                        % (serial_number, GROUP_NAME, type_name, SOLUTION_NAME))):
                    raise Cancelled()
                status("Creating %s..." % serial_number)
                device = doover.create_device(serial_number, kind)
                self.log("Created %s (agent %s)" % (device.get("display_name"), device["id"]))
                doover.ensure_solution(device["id"], self.log)
                step(1, "ok")

            step(2)
            status("Downloading certfile...")
            raw = doover.download_certfile(device["id"])
            step(2, "ok")

            step(3)
            status("Adding root CA...")
            certfile = build_certfile(raw, serial_number, self.log)
            step(3, "ok")

            step(4)
            status("Flashing on %s..." % port)
            cancelled = self.cancel_flag.is_set
            if kind == "cell":
                flash_cell(port, certfile, self.log, cancelled)
            else:
                flash_wifi(port, certfile, self.log, cancelled, progress=status)
            step(4, "ok")
            status("✓  %s provisioned. It connects to Doover on its next wake." % serial_number, "ok")
            self.log("Done. The device connects to %s on its next wake." % certfile["BROKER_ENDPOINT"])
        except Cancelled:
            self.post("step", current[0], "fail")
            status("Stopped. Nothing more was done.", "warn")
            self.log("Stopped by the operator.")
        except LoginRequired as e:
            self.post("step", current[0], "fail")
            status("Not logged in to Doover", "fail")
            self.log("ERROR: %s" % e)
            if self.ask(lambda: messagebox.askyesno(
                    "Doover login", "%s\n\nLog in now? A window opens for the Doover login; approve it in the "
                    "browser, then press Provision again." % e)):
                self.ask(start_doover_login)
        except ProvisionError as e:
            self.post("step", current[0], "fail")
            status("Failed at: %s" % STEP_DETAIL[current[0]], "fail")
            self.log("ERROR: %s" % e)
            self.ask(lambda: messagebox.showerror("Provisioning failed", str(e)))
        except Exception as e:
            self.post("step", current[0], "fail")
            status("Failed at: %s" % STEP_DETAIL[current[0]], "fail")
            self.log(traceback.format_exc())
            self.ask(lambda: messagebox.showerror("Provisioning failed", "%s: %s" % (type(e).__name__, e)))
        finally:
            self.post("done")


def start_doover_login():
    """Opens `doover login` in its own console window. Runs on the UI thread."""
    doover = shutil.which("doover")
    uv_tool_exe = os.path.expanduser(r"~\.local\bin\doover.exe")  # uv tool install's default location
    if not doover and os.path.exists(uv_tool_exe):
        doover = uv_tool_exe
    if not doover:
        messagebox.showerror("Doover login", "The Doover CLI isn't installed on this PC. Install it once with:\n\n"
                             "  uv tool install doover-cli\n\nor\n\n  python -m pip install doover-cli\n\n"
                             "then press Provision again.")
        return
    subprocess.Popen([doover, "login"], creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))


class ExistsDialog:
    """Device already exists: do nothing, close the program, or pull its certfile and provision."""

    def __init__(self, parent, device, expected_type_id):
        self.result = "nothing"
        top = self.top = tk.Toplevel(parent, bg=CARD)
        top.title("Device already exists")
        top.transient(parent)
        top.resizable(False, False)

        band = tk.Frame(top, bg=STATE_COLOURS["warn"][1], padx=20, pady=14)
        band.pack(fill="x")
        tk.Label(band, text="%s is already on Doover" % device.get("display_name"), font=(FONT, 12, "bold"),
                 bg=STATE_COLOURS["warn"][1], fg=TEXT).pack(anchor="w")

        frame = tk.Frame(top, bg=CARD, padx=20, pady=14)
        frame.pack(fill="both", expand=True)
        device_type = device.get("type") or {}
        group = device.get("group") or {}
        grid = tk.Frame(frame, bg=CARD)
        grid.pack(anchor="w")
        for row, (label, value) in enumerate((("Group", group.get("name", "?")), ("Type", device_type.get("name", "?")),
                                              ("Agent ID", device.get("id")))):
            tk.Label(grid, text=label, font=(FONT, 9), bg=CARD, fg=MUTED).grid(row=row, column=0, sticky="w",
                                                                              padx=(0, 16), pady=1)
            tk.Label(grid, text=value, font=(FONT, 10), bg=CARD, fg=TEXT).grid(row=row, column=1, sticky="w", pady=1)
        if str(device_type.get("id")) != str(expected_type_id):
            tk.Label(frame, text="⚠  Its device type doesn't match the one you selected. Check it's the right "
                     "device.", font=(FONT, 9, "bold"), bg=CARD, fg=STATE_COLOURS["warn"][0], wraplength=440,
                     justify="left").pack(anchor="w", pady=(10, 0))
        tk.Label(frame, text="Pulling its certfile adds a new MQTT key to the device. Spears already using an "
                 "older key keep working.", font=(FONT, 9), bg=CARD, fg=MUTED, wraplength=440,
                 justify="left").pack(anchor="w", pady=(10, 14))

        buttons = tk.Frame(frame, bg=CARD)
        buttons.pack(fill="x")
        FlatButton(buttons, "Pull certfile and provision", lambda: self.pick("provision")).pack(side="left")
        FlatButton(buttons, "Do nothing", lambda: self.pick("nothing"), primary=False).pack(side="left", padx=8)
        FlatButton(buttons, "Close program", lambda: self.pick("close"), primary=False).pack(side="left")

        top.protocol("WM_DELETE_WINDOW", lambda: self.pick("nothing"))
        top.bind("<Escape>", lambda e: self.pick("nothing"))
        top.grab_set()
        top.update_idletasks()
        x = parent.winfo_rootx() + (parent.winfo_width() - top.winfo_width()) // 2
        y = parent.winfo_rooty() + (parent.winfo_height() - top.winfo_height()) // 3
        top.geometry("+%d+%d" % (max(x, 0), max(y, 0)))
        parent.wait_window(top)

    def pick(self, value):
        self.result = value
        self.top.destroy()


def main():
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # Sharp text on high-DPI screens
    except Exception:
        pass
    root = tk.Tk()
    # Opened by double click there is no console, so errors from UI callbacks would otherwise vanish
    root.report_callback_exception = lambda *exc: messagebox.showerror(
        "GrainWatch V2 Provisioning", "".join(traceback.format_exception(*exc))[-1500:])
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
