obj-m += ax99100_lpt.o

INSTALL_DIR = /lib/modules/$(shell uname -r)/kernel/drivers/gpio

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	mkdir -p $(INSTALL_DIR)
	cp ax99100_lpt.ko $(INSTALL_DIR)/
	depmod -a
	lspci -nnk -s 21:00.2
	modprobe ax99100_lpt
	chmod 666 /dev/ax99100_lpt

#	rmmod ppdev
#       rmmod lp
#       rmmod parport_pc
#       rmmod parport

# echo "0000:21:00.2" | sudo tee /sys/bus/pci/drivers/parport_pc/unbind
# echo "0000:21:00.2" | sudo tee /sys/bus/pci/drivers/ax99100_lpt/bind

# sudo nano /etc/modprobe.d/ax99100_priority.conf
# 1. Запрещаем parport_pc автоматически перехватывать это устройство
# blacklist parport_pc
# 2. Устанавливаем жесткий приоритет загрузки
# Перед тем как ядро попытается загрузить parport_pc (если его вызовет другая система),
# оно ОБЯЗАНО сначала загрузить ваш кастомный драйвер
# softdep parport_pc pre: ax99100_lpt

# sudo nano /etc/udev/rules.d/99-ax99100.rules
# KERNEL=="ax99100_lpt", MODE="0666"


uninstall:
	rmmod ax99100_lpt
	rm -f $(INSTALL_DIR)/ax99100_lpt.ko
	depmod -a
