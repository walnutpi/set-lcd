
## 关于本文件夹
在运行`set-lcd`指令时,指令的第2个参数就是这里面文件夹的名称。文件夹内存放驱动源码，设备树等

## tft
- `TFT_DTS_NAME` 变量的值会赋值给config.txt里面的overlays参数。
- `TFT_MODULE` 变量的值是驱动名称。
- `USE_CONF_FILE` 要使用哪些配置文件
## drivers
存放驱动源文件，需要makefile提供 编译 clean output 3种功能。output时需要将编译结果输出到`drivers-save`文件夹

## drivers-save
在各内核版本下编译好的驱动,`set-lcd`运行的时候会先看这里面也没有保存，没有就去drivers文件夹下当场运行编译


## dtb
设备树文件，注意`set-lcd`命令的设置旋转方向功能，是通过修改设备树内的这一项来实现的
```
				rotate = <270>;
```