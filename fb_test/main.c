#include <stdio.h>
#include <sys/types.h> //open需要的头文件
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> //write
#include <sys/types.h>
#include <sys/mman.h> //mmap  内存映射相关函数库
#include <stdlib.h>   //malloc free 动态内存申请和释放函数头文件
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <stdint.h>

// 32位的颜色
#define Black 0x0000
#define White 0xffff
#define Red 0xf800
#define Green 0x0fc0
#define Blue 0x001f

int fd;
uint16_t *fb_mem = NULL; // 设置显存的位数为32位
struct fb_var_screeninfo var;
struct fb_fix_screeninfo fix;

int main(void)
{
    unsigned int i;
    int ret;

    /*--------------第一步--------------*/
    fd = open("/dev/fb0", O_RDWR); // 打开framebuffer设备
    if (fd == -1)
    {
        perror("Open LCD");
        return -1;
    }
    /*--------------第二步--------------*/

    // 获取屏幕的可变参数
    ioctl(fd, FBIOGET_VSCREENINFO, &var);
    // 获取屏幕的固定参数
    ioctl(fd, FBIOGET_FSCREENINFO, &fix);

    // 打印分辨率
    printf("xres= %d,yres= %d \n", var.xres, var.yres);
    // 打印总字节数和每行的长度
    printf("line_length=%d,smem_len= %d \n", fix.line_length, fix.smem_len);
    printf("xpanstep=%d,ypanstep= %d \n", fix.xpanstep, fix.ypanstep);

    /*--------------第三步--------------*/

    fb_mem = (uint16_t *)mmap(NULL, var.xres * var.yres * 4, // 获取显存，映射内存
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (fb_mem == MAP_FAILED)
    {
        perror("Mmap LCD");
        return -1;
    }

    memset(fb_mem,0xff,var.xres*var.yres*2);		//清屏

    sleep(1);
    /*--------------第四步--------------*/
    // //将屏幕全部设置成蓝色
    // for(i=0;i< var.xres*var.yres ;i++)
    // 	fb_mem[i] = Blue;
    // 将屏幕全部设置成蓝色
    // for(i=480*10;i< 480*12 ;i++)
    for (i = 0 + 480 * 300; i < 480 * 2 + 480 * 300; i++)
        fb_mem[i] = Red;

    sleep(2);

    munmap(fb_mem, var.xres * var.yres * 4); // 映射后的地址，通过mmap返回的值
    close(fd);                               // 关闭fb0设备文件
    return 0;
}