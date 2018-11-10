mkdir modout
make whyred_kali_defconfig
make INSTALL_MOD_PATH=modout
make INSTALL_MOD_PATH=modout INSTALL_MOD_STRIP=1 modules_install
echo "Job Finished X)"
