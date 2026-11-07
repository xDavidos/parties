#!/bin/bash
# Grants the microphone and the camera to a bundle id. Capture device init
# blocks on the prompt, which nobody answers on a CI runner. GitHub's runner
# VMs let root write the TCC databases; elsewhere, answer the prompts by hand.
#
#   grant_tcc.sh <bundle id>
set -uo pipefail

bundle_id=$1
now=$(date +%s)
for db in "$HOME/Library/Application Support/com.apple.TCC/TCC.db" \
          "/Library/Application Support/com.apple.TCC/TCC.db"; do
    for service in kTCCServiceMicrophone kTCCServiceCamera; do
        if sudo sqlite3 "$db" "INSERT OR REPLACE INTO access
                (service, client, client_type, auth_value, auth_reason, auth_version, flags, last_modified)
                VALUES ('$service', '$bundle_id', 0, 2, 3, 1, 0, $now);"; then
            echo "TCC grant $service in $db: inserted"
        else
            echo "TCC grant $service in $db: insert failed"
            sudo sqlite3 "$db" ".schema access" || true
        fi
        echo "    row: $(sudo sqlite3 "$db" "SELECT service, client, client_type, auth_value, auth_reason
            FROM access WHERE service = '$service' AND client = '$bundle_id';" 2>&1)"
    done
done
sudo pkill -x tccd || true
