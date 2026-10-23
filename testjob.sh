#!/bin/bash
# Submits a small test job (4 GPU nodes, 5 minutes) to see RSV's node view on a real allocation.
# Usage: ./testjob.sh <account>        (your project, as in sbatch --account; u in RSV lists yours)

set -e

ACCOUNT=${1:-}
if [ -z "$ACCOUNT" ]; then
    echo "Usage: $0 <account>" >&2
    exit 1
fi

sbatch --account="$ACCOUNT" <<'EOF'
#!/bin/bash
#SBATCH --nodes=4
#SBATCH --ntasks=8
#SBATCH -c 2
#SBATCH --time=00:05:00
#SBATCH --constraint=armgpu
#SBATCH --mem=1G
#SBATCH --gpus-per-node=2
#SBATCH --job-name=test_rsv

sleep 120
EOF
