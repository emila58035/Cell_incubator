#!/usr/bin/env bash
#
# Incubator server-side installer (Ubuntu). Idempotent: safe to re-run after a
# code update. Nothing outside the paths listed in README.md "What gets written"
# is modified.
#
#   sudo bash server/deploy/setup.sh               # API only, 127.0.0.1:8000
#   sudo bash server/deploy/setup.sh --with-nginx  # + self-signed cert, 443
#
# The default run deliberately touches no public port: it installs the API,
# starts it on loopback and verifies it with the contract smoke test. The 443
# front end is a second, separate run so that a failure there cannot affect the
# API and so the Nginx change is always reviewed on its own.
#
# Never touched: /etc/nginx/conf.d/ota.conf (the /fw/ OTA service), the 80-port
# stub in incubator.conf, /root, /www/server/panel.

set -euo pipefail

WITH_NGINX=0
for arg in "$@"; do
  case "$arg" in
    --with-nginx) WITH_NGINX=1 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR=/opt/incubator/app
DATA_DIR=/var/lib/incubator
CONF_DIR=/etc/incubator
CERT_DIR=$CONF_DIR/certs
TOKEN_FILE=$CONF_DIR/devices.json
UNIT=/etc/systemd/system/incubator-api.service
HTPASSWD=/etc/nginx/incubator.htpasswd
NGINX_CONF=/etc/nginx/conf.d/incubator-https.conf
SERVER_IP=175.24.182.190
SMOKE_FAILED=0

log() { printf '\n=== %s ===\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run with sudo"
[ -f "$SRC/app.py" ] || die "app.py not found in $SRC - run this from the unpacked bundle"
[ -f "$SRC/schema.sql" ] || die "schema.sql not found in $SRC"
python3 -m venv --help >/dev/null 2>&1 || die "python3-venv is missing: apt-get install -y python3-venv"
OLD_UMASK="$(umask)"

log "system user and directories"
if ! id -u incubator >/dev/null 2>&1; then
  useradd --system --home-dir /opt/incubator --shell /usr/sbin/nologin incubator
  echo "created system user incubator"
else
  echo "system user incubator already exists"
fi
install -d -o incubator -g incubator -m 0755 "$APP_DIR"
install -d -o incubator -g incubator -m 0750 "$DATA_DIR"
install -d -o root      -g incubator -m 0750 "$CONF_DIR"
install -d -o root      -g root      -m 0700 "$CERT_DIR"

log "application code"
install -o incubator -g incubator -m 0644 "$SRC/app.py"      "$APP_DIR/app.py"
install -o incubator -g incubator -m 0644 "$SRC/camera_api.py" "$APP_DIR/camera_api.py"
install -o incubator -g incubator -m 0644 "$SRC/schema.sql"  "$APP_DIR/schema.sql"
[ -f "$SRC/README.md" ] && install -o incubator -g incubator -m 0644 "$SRC/README.md" "$APP_DIR/README.md"
rm -rf "$APP_DIR/static" "$APP_DIR/tests"
install -d -o incubator -g incubator -m 0755 "$APP_DIR/static"
install -o incubator -g incubator -m 0644 "$SRC/static/index.html" "$APP_DIR/static/index.html"
install -o incubator -g incubator -m 0644 "$SRC/static/camera.html" "$APP_DIR/static/camera.html"
install -d -o incubator -g incubator -m 0755 "$APP_DIR/tests"
install -o incubator -g incubator -m 0644 "$SRC/tests/test_api_smoke.py" "$APP_DIR/tests/test_api_smoke.py"
install -o incubator -g incubator -m 0644 "$SRC/tests/test_websocket_control.py" "$APP_DIR/tests/test_websocket_control.py"
echo "installed app.py, schema.sql, static/index.html, tests/"

log "virtualenv and dependencies"
if [ ! -x "$APP_DIR/venv/bin/python" ]; then
  python3 -m venv "$APP_DIR/venv"
  echo "created venv"
else
  echo "reusing existing venv"
fi
"$APP_DIR/venv/bin/pip" install --quiet --disable-pip-version-check --upgrade pip \
  || echo "pip self-upgrade skipped (not required, continuing)"
if ! "$APP_DIR/venv/bin/pip" install --quiet --disable-pip-version-check fastapi uvicorn websockets; then
  echo "PyPI not reachable directly; retrying with the Tsinghua mirror"
  "$APP_DIR/venv/bin/pip" install --quiet --disable-pip-version-check \
    -i https://pypi.tuna.tsinghua.edu.cn/simple fastapi uvicorn websockets
fi
chown -R incubator:incubator "$APP_DIR/venv"
"$APP_DIR/venv/bin/python" -c 'import fastapi, uvicorn, websockets; print("fastapi", fastapi.__version__, "/ uvicorn", uvicorn.__version__, "/ websockets", websockets.__version__)'

log "device token"
if [ -s "$TOKEN_FILE" ]; then
  echo "kept the existing $TOKEN_FILE (tokens are never rotated automatically)"
else
  TOKEN="$(openssl rand -hex 32)"
  umask 077
  printf '{\n  "incubator-001": { "token": "%s", "label": "\xe5\x9f\xb9\xe5\x85\xbb\xe7\xae\xb1 1 \xe5\x8f\xb7" }\n}\n' "$TOKEN" > "$TOKEN_FILE"
  umask "$OLD_UMASK"
  unset TOKEN
  echo "generated a new device token in $TOKEN_FILE"
fi
chown root:incubator "$TOKEN_FILE"
chmod 0640 "$TOKEN_FILE"
echo "read it when the firmware needs it with:  sudo cat $TOKEN_FILE"
echo "never paste it into a chat, an issue or a commit."

log "database"
if [ -f "$DATA_DIR/data.db" ]; then
  echo "database exists; re-applying schema.sql (all statements are idempotent)"
else
  echo "creating a new database"
fi
"$APP_DIR/venv/bin/python" - "$DATA_DIR/data.db" "$APP_DIR/schema.sql" <<'PY'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.executescript(open(sys.argv[2], encoding="utf-8").read())
conn.commit()
print("  journal_mode =", conn.execute("PRAGMA journal_mode").fetchone()[0])
print("  tables       =", ", ".join(
    r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")))
conn.close()
PY
chown -R incubator:incubator "$DATA_DIR"

log "systemd unit"
install -o root -g root -m 0644 "$SRC/deploy/incubator-api.service" "$UNIT"
systemctl daemon-reload
systemctl enable incubator-api >/dev/null
systemctl restart incubator-api
sleep 1
systemctl is-active incubator-api

log "local health check (loopback only)"
HEALTH=/tmp/incubator_health.json
for _ in $(seq 1 20); do
  if curl -fsS --max-time 2 http://127.0.0.1:8000/healthz >"$HEALTH" 2>/dev/null; then break; fi
  sleep 0.5
done
cat "$HEALTH" 2>/dev/null || die "no answer on 127.0.0.1:8000 - see: journalctl -u incubator-api -n 50"
echo

log "contract smoke test"
# Run as the service account, not as root: that way the test also proves the
# incubator user can read the app directory and the virtualenv.
SMOKE_RUNNER="runuser -u incubator --"
command -v runuser >/dev/null 2>&1 || SMOKE_RUNNER="sudo -u incubator --"
if $SMOKE_RUNNER "$APP_DIR/venv/bin/python" "$APP_DIR/tests/test_api_smoke.py"; then
  echo "smoke test passed"
else
  SMOKE_FAILED=1
  echo "smoke test FAILED - the API is running but does not behave as specified"
fi

if [ "$WITH_NGINX" -eq 1 ]; then
  log "self-signed CA and IP certificate"
  if [ -f "$CERT_DIR/server.pem" ] && [ -f "$CERT_DIR/incubator-ca.pem" ]; then
    echo "keeping existing certificates in $CERT_DIR (delete them to re-issue)"
  else
    umask 077
    openssl genrsa -out "$CERT_DIR/incubator-ca.key" 4096 2>/dev/null
    openssl req -x509 -new -key "$CERT_DIR/incubator-ca.key" -sha256 -days 3650 \
      -subj "/CN=Incubator Local CA" -out "$CERT_DIR/incubator-ca.pem"
    openssl genrsa -out "$CERT_DIR/server.key" 2048 2>/dev/null
    openssl req -new -key "$CERT_DIR/server.key" -subj "/CN=incubator-server" \
      -out "$CERT_DIR/server.csr"
    openssl x509 -req -in "$CERT_DIR/server.csr" \
      -CA "$CERT_DIR/incubator-ca.pem" -CAkey "$CERT_DIR/incubator-ca.key" \
      -CAcreateserial -days 825 -sha256 -out "$CERT_DIR/server.pem" \
      -extfile <(printf 'subjectAltName=IP:%s\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' "$SERVER_IP")
    rm -f "$CERT_DIR/server.csr"
    umask "$OLD_UMASK"
    chmod 0600 "$CERT_DIR"/*.key
    chmod 0644 "$CERT_DIR"/*.pem
    echo "issued a new CA and IP certificate"
  fi
  echo "--- certificate identity (subjectAltName MUST be an IPAddress) ---"
  openssl x509 -in "$CERT_DIR/server.pem" -noout -subject -dates -ext subjectAltName
  echo "BACK UP $CERT_DIR/incubator-ca.key now: it is the only way to re-issue the"
  echo "server certificate without reflashing the firmware, and it must never be"
  echo "copied into the repository or into the firmware."

  log "dashboard password"
  if [ -s "$HTPASSWD" ]; then
    echo "keeping the existing $HTPASSWD (delete it to generate a new password)"
  else
    PASSWORD="$(openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | cut -c1-20)"
    HASH="$(printf '%s' "$PASSWORD" | openssl passwd -apr1 -stdin)"
    umask 077
    printf 'incubator:%s\n' "$HASH" > "$HTPASSWD"
    umask "$OLD_UMASK"
    echo "  username: incubator"
    echo "  password: $PASSWORD"
    echo "  >>> store this in your password manager NOW; only the apr1 hash is kept."
  fi
  NGINX_USER="$(awk '/^[[:space:]]*user[[:space:]]/{print $2; exit}' /etc/nginx/nginx.conf | tr -d ';')"
  NGINX_USER="${NGINX_USER:-www-data}"
  # The nginx worker reads this file as $NGINX_USER, which is why it does not
  # live under /etc/incubator (0750 root:incubator) - an unreadable htpasswd
  # produces a 403 with no useful explanation.
  chown root:"$NGINX_USER" "$HTPASSWD"
  chmod 0640 "$HTPASSWD"
  echo "htpasswd readable by user $NGINX_USER"

  log "nginx 443 front end"
  if ! nginx -t >/dev/null 2>&1; then
    echo "WARNING: nginx -t already fails BEFORE this change; investigate separately"
  fi
  install -o root -g root -m 0644 "$SRC/deploy/incubator-https.conf" "$NGINX_CONF"
  if nginx -t; then
    systemctl reload nginx
    echo "nginx reloaded with $NGINX_CONF"
  else
    echo "nginx -t FAILED - removing $NGINX_CONF again"
    rm -f "$NGINX_CONF"
    nginx -t || true
    die "Nginx configuration was not applied; the previous configuration is intact"
  fi
fi

log "summary"
systemctl --no-pager --lines=0 status incubator-api | head -5 || true
echo
echo "API        http://127.0.0.1:8000  (loopback only, never exposed directly)"
echo "token      $TOKEN_FILE"
echo "database   $DATA_DIR/data.db"
echo "logs       journalctl -u incubator-api -f"
if [ "$WITH_NGINX" -eq 1 ]; then
  echo "dashboard  https://$SERVER_IP/   (browser will warn: self-signed CA)"
  echo "nginx conf $NGINX_CONF   (rollback: rm it, then systemctl reload nginx)"
else
  echo "dashboard  not exposed yet - re-run with --with-nginx to add the 443 front end"
fi
echo
echo "Reset the dashboard password:"
echo "  P=\$(openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | cut -c1-20); \\"
echo "  printf '%s' \"\$P\" | openssl passwd -apr1 -stdin > /etc/nginx/incubator.htpasswd; \\"
echo "  chown root:\${NGINX_USER:-www-data} /etc/nginx/incubator.htpasswd; chmod 0640 /etc/nginx/incubator.htpasswd; echo \"\$P\""
echo

[ "$SMOKE_FAILED" -eq 0 ] || die "install finished but the contract smoke test failed"
