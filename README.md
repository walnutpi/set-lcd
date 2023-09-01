# set-lcd
对于插在派上的lcd屏进行设置。从基本的安装，到设置旋转，以及是否作为控制台等功能。

# 安装
1. clone
```
git clone https://github.com/sc-bin/set-lcd.git
```

2. 执行安装脚本
```
sudo ./install
```
注意安装完不要删除本文件夹

3. 重启
第一次安装完，需要重启后才能让自动补全脚本生效，如果不想重启，输入以下命令即可
```
source /etc/bash_completion.d/set-lcd
```


# 使用
```
set-lcd [lcd-name] [function] [value]
```
命令后面带的参数都支持自动补全，建议按两下tab键来查看有什么可输入的
- `[lcd-name]` 选择你的lcd屏
- `[function]` 要运行的功能
- `[value]` 有些功能需要再附带一些输入参数

## 启用lcd屏，并配置为桌面
开机时如果不插hdmi，则用这个屏幕作为桌面，如果插上hdmi，则把桌面显示到hdmi屏那边。重启后生效
```
set-lcd lcd35-st7796 install_desktop
```

## 仅启用lcd屏，不做桌面
适用于不想让桌面占用lcd屏，想自己控制lcd屏显示其他内容。可以使用/dev/fb节点来操作屏幕。重启后生效
```
set-lcd lcd35-st7796 install_only_fb
```

## 旋转lcd方向
从驱动层面对lcd的方向进行旋转，重启后生效
```
set-lcd lcd35-st7796 set_rotate 180
```

## 禁用lcd
lcd屏的会占用引脚资源，如果不想使用lcd屏了，运行这个命令，禁用驱动。重启后生效。
```
set-lcd lcd35-st7796 remove
```
