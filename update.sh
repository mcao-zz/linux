#!/bin/bash

set -e

# Reading sources
. ./iKOTD/mapping.sh

: Reading options
while getopts 'd:n:e:f:' opt
do
	case ${opt} in
		d) DISTRO="${OPTARG}" ;;
		n) USERName="${OPTARG}" ;;
		e) USEREmail="${OPTARG}" ;;
		f) FTP3="${OPTARG:-FTP3}";;
		*);;
	esac
done

set -x

if [[ ! -n "${DISTRO}" ]] ; then
	echo "Wrong -d was not set."
	exit 1
fi
if [[ ! -n "${USERName}" ]]; then
	echo "Wrong -n was not set."
	exit 1
fi
if [[ ! -n "${USEREmail}" ]]; then
	echo "Wrong -e was not set."
	exit 1
fi
if [[ ! -n "${FTP3}" ]]; then
	echo "Wrong -f was not set."
	exit 1
fi

git config --global user.email "${USEREmail}"
git config --global user.name "${USERName}"
git fetch --all

: Checking if the remote branch is already added
if git branch          | grep "${ftp3[${DISTRO}]}" &>/dev/null; then
	if git branch | grep "^\* ${ftp3[${DISTRO}]}" &>/dev/null; then
		: Current branch
	fi
	: Change to another branch which already exist
	git checkout ${ftp3[${DISTRO}]}
elif ! git branch -avv | grep "remotes/origin/${ftp3[${DISTRO}]}" &>/dev/null; then
	: Creating new branch
	git checkout -b ${ftp3[${DISTRO}]} origin/master
	git push -u origin ${ftp3[${DISTRO}]}
else
	git checkout -b ${ftp3[${DISTRO}]} origin/master
	git push -u origin ${ftp3[${DISTRO}]}
fi

ROOT="$(pwd)"
RH_PASSPORT="${FTP3}" bash -x ./iKOTD/get_latest.sh ${ftp3[${DISTRO}]}

: Looking for the latest downloaded file for build
for i in $(find ${ROOT} -iname 'kernel*.src.rpm');
do
	KRN_SRC_Filename="${i}"
done

KRN_SRC_Filename="$(echo ${KRN_SRC_Filename} | rev | cut -d'/' -f1 | rev)"

if [[ ! -n "${KRN_SRC_Filename}" ]]; then
	echo "Kernel source was not found."
	exit 1
fi

: Checking if package was already committed
if git log --oneline  | grep "${KRN_SRC_Filename}" > /dev/null; then
	echo "It's already added"
	exit 0
fi


: Define the topdir for rpm
echo '%_topdir %(echo $HOME)/rpmbuild' >> ~/.rpmmacros

: Install rpm source
rpm -ivh ${KRN_SRC_Filename}

cd ${HOME}/rpmbuild/
if [[ "${DISTRO}" =~ .*rhel.* ]]; then
	cd ${HOME}/rpmbuild/SPECS
	rpmbuild -bp kernel.spec
else
	cd SOURCES
	./mkspec
	mv kernel-default.spec ${HOME}/rpmbuild/SPECS
	cd ${HOME}/rpmbuild/SPECS
	rpmbuild -bp kernel-default.spec
	rm -rf ${ROOT}/source
	mv ${HOME}/rpmbuild/BUILD/kernel-default-*/linux-*/ ${ROOT}/
	cd ${ROOT}
	mv linux-* source
fi

cd "${ROOT}"
git add source
git commit -s -a -m "[${ftp3[${DISTRO}]}] ${KRN_SRC_Filename}"
git push




