#!/usr/bin/env bash
set -euo pipefail

pages_dir="/sys/kernel/mm/hugepages/hugepages-1048576kB"
nr_file="$pages_dir/nr_hugepages"
free_file="$pages_dir/free_hugepages"
grub_cmdline="default_hugepagesz=1G hugepagesz=1G hugepages=1"

if [[ ! -e "$nr_file" || ! -e "$free_file" ]]; then
    echo "1 GiB hugetlb control files are not available on this kernel."
    echo "Set this GRUB kernel command line and reboot:"
    echo "  $grub_cmdline"
    exit 1
fi

echo "This will request sudo and try to reserve one 1 GiB hugetlb page."
sudo -v

current=$(<"$nr_file")
if (( current < 1 )); then
    echo 1 | sudo tee "$nr_file" >/dev/null
fi

free=$(<"$free_file")
if (( free >= 1 )); then
    echo "1 GiB hugepages reserved: $(<"$nr_file"), free: $free"
    exit 0
fi

cat <<EOF
Unable to reserve a free 1 GiB hugetlb page at runtime.

If runtime reservation keeps failing, set this GRUB kernel command line and reboot:
  $grub_cmdline

Typical Debian/Ubuntu flow:
  sudoedit /etc/default/grub
  sudo update-grub
EOF

exit 1
