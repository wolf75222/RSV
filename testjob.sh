sbatch <<'EOF'
#!/bin/bash
#SBATCH --nodes=4
#SBATCH --ntasks=8
#SBATCH -c 2
#SBATCH --time=00:05:00
#SBATCH --account=r250127
#SBATCH --constraint=armgpu
#SBATCH --mem=1G
#SBATCH --gpus-per-node=2
#SBATCH --job-name=test_rsv

sleep 120
EOF