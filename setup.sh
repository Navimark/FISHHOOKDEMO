#!/bin/sh

submoduleName=$(git config --file .gitmodules --get-regexp path | awk '{ print $2 }')
if [[ $submoduleName == "spacecommander" ]]; then
	echo -e "\033[32m >> updating spacecommander☕️ \033[0m"
	git submodule init
	git submodule update
else
	echo -e "\033[32m >> installing spacecommander☕️ \033[0m"
	git submodule add git@git.moumentei.com:appforce/spacecommander.git	
fi

if [ $? -eq 0 ]; then
	echo -e "\033[32m >> integrating spacecommander ☕ ️[\033[0m"
	sh ./spacecommander/setup-repo.sh
	echo -e "\033[32m >> spacecommander install done ✔️\n \033[0m"
else
	echo -e "\033[31m >> install failed X\n \033[0m"
fi