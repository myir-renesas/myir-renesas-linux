#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/platform_device.h>

struct gpio_export_gpio{
    char *name; 
    struct gpio_desc *desc;
};

struct gpio_export{
    struct device *dev;
    int gpios_num;
    struct gpio_export_gpio *gpios; 
};

static int gpio_export_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct device_node *cnp; 
    int nb = 0;
    int val;

    struct gpio_export *ge;
    struct device *dev = &pdev->dev;
    
    ge = devm_kzalloc(dev, sizeof(struct gpio_export), GFP_KERNEL);
    if (IS_ERR(ge))
        return PTR_ERR(ge);

    ge->dev = dev;
    dev_set_drvdata(dev, ge);

    for_each_child_of_node(np, cnp) {
        ++nb;
    }

    ge->gpios_num = nb;
    nb = 0;
    ge->gpios = devm_kzalloc(dev, 
                            sizeof(struct gpio_export_gpio) * ge->gpios_num,
                            GFP_KERNEL);
    if (IS_ERR(ge->gpios)) 
        return PTR_ERR(ge->gpios);    
    
    for_each_child_of_node(np, cnp) { 
        const char *name = NULL;
        int gpio;
        bool dmc;

        of_property_read_string(cnp, "gpio-export,name", &name);
        if (!name) {
            name = of_node_full_name(np);
        }

        ge->gpios[nb].name = devm_kzalloc(dev, strlen(name) + 1, GFP_KERNEL);
        strncpy(ge->gpios[nb].name, name, strlen(name));
        gpio = of_get_gpio(cnp, 0);
        ge->gpios[nb].desc = gpio_to_desc(gpio);

        if (devm_gpio_request(&pdev->dev, gpio,ge->gpios[nb].name))
	{
            ++nb;
            continue;
	}
	
        if (!of_property_read_u32(cnp, "gpio-export,output", &val))
            gpio_direction_output(gpio, val);
        else
	    gpio_direction_output(gpio, val);
            //gpio_direction_input(gpio);
        dmc = of_property_read_bool(cnp, "gpio-export,direction-may-change");
        gpiod_export(ge->gpios[nb].desc, dmc);
        gpiod_export_link(&pdev->dev, ge->gpios[nb].name, ge->gpios[nb].desc);

        ++nb;
    }

    dev_info(&pdev->dev, "%d gpio(s) exported\n", nb);

    return 0;
}

static int gpio_export_remove(struct platform_device *pdev)
{   
    struct device *dev = &pdev->dev;
    struct gpio_export *ge = dev_get_drvdata(dev);
    int i;

    for (i = 0; i < ge->gpios_num; i++) {
        sysfs_remove_link(&ge->dev->kobj, ge->gpios[i].name);
        gpiod_unexport(ge->gpios[i].desc);
    }

    return 0;
}
static struct of_device_id gpio_export_ids[] = {
	{ .compatible = "linux-gpio-select" },
};

static struct platform_driver gpio_export_driver = {
	.driver = {
		.name = "linux-gpio-select",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gpio_export_ids),
		},
	.probe = gpio_export_probe,
	.remove = gpio_export_remove,
};

static int __init gpio_export_init(void)
{
    return platform_driver_register(&gpio_export_driver);
}
 
static void __exit gpio_export_exit(void)
{
    platform_driver_unregister(&gpio_export_driver);
}

module_init(gpio_export_init);
module_exit(gpio_export_exit);
MODULE_LICENSE("GPL");
