podman run --security-opt seccomp=unconfined -v $(pwd):/root/kernels -w /root/kernels/ -it ${1}/${2} /root/kernels/.update -d ${2} -n "${3}" -e "${4}" -f "${5}"
