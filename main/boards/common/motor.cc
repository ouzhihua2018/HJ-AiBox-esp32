#include "motor.h"
#define TAG "MOTOR"
motor::motor()
{

}

motor::~motor()
{
}

void motor::InitMotor(gpio_num_t MOTOR_PWM_GPIO)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 4883,
        .clk_cfg = LEDC_AUTO_CLK};

    esp_err_t timer_ret = ledc_timer_config(&timer_conf);
    if (timer_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LEDC定时器初始化失败：%s", esp_err_to_name(timer_ret));
        return;
    }
    ledc_channel_config_t ledc_conf;
    ledc_conf.channel = LEDC_CHANNEL_1;
    ledc_conf.duty = 0;
    ledc_conf.gpio_num = MOTOR_PWM_GPIO;
    ledc_conf.intr_type = LEDC_INTR_DISABLE;
    ledc_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_conf.timer_sel = timer_conf.timer_num;
    ledc_conf.sleep_mode =  LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    ledc_conf.hpoint = 0;
    ledc_conf.flags = {
        .output_invert = 0};
    ledc_channel_config(&ledc_conf);
    ESP_LOGI(TAG, "ledc_channel_config");
    // add mcp tools
    auto &mcp_server = McpServer::GetInstance();
    // 启动电机（开始旋转）
mcp_server.AddTool("self.motor.start", 
    "Start the motor at a fixed medium speed (5000/8191, ~61% speed).\n"
    "当用户说:开始旋转、启动、打开电机、旋转、转动时你必须调用该工具:\n"
    "Note: The motor uses LEDC 13-bit resolution (0-8191), 5000 is a medium speed.", 
    PropertyList(), 
    [this](const PropertyList &properties) -> ReturnValue
    {
        working_ = true;
        uint32_t target_duty = 5000;
        // 安全校验：防止极端情况越界
        target_duty = std::min(target_duty, (uint32_t)8191);
        
        ESP_LOGI(TAG, "收到启动MCP，设置目标占空比%ld", target_duty);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        return true; 
    });

// 停止电机（停止旋转）
mcp_server.AddTool("self.motor.stop", 
    "Stop the motor immediately (halt rotation completely).\n"
    "Use this tool for:\n"
    "当用户说:停止、停止旋转、停止转动、关闭电机、别转了时你必须调用该工具:\n", 
    PropertyList(), 
    [this](const PropertyList &properties) -> ReturnValue
    {
        working_ = false;
        uint32_t target_duty = 0;
        
        ESP_LOGI(TAG, "收到停止MCP，设置目标占空比%ld", target_duty);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        return true; 
    });

    // 加速（固定档位提升，用户无需输入）
    mcp_server.AddTool("self.motor.speed_up",
    "1. The user uses Chinese/English phrases to request faster motor speed, such as: \n"
    "   - Chinese: 加速、转快点、转快一点、快一点、快点、转速调高、提高转速、转得快些、加快速度\n"
    "   - English: 'speed up', 'faster', 'go faster', 'increase motor speed', 'more speed', 'raise rotation speed'\n"
    "2. Gradually make the motor run faster.",
    PropertyList(),
    [this](const PropertyList &properties) -> ReturnValue
    {
        uint32_t current = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        uint32_t step = 1000;      // 每次加速步长
        uint32_t max_duty = 8191;  // 13位最大占空比
        uint32_t target_duty = current + step;
        
        if (target_duty > max_duty)
            target_duty = max_duty;
        
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        working_ = (target_duty > 0);
        
        ESP_LOGI(TAG, "MCP指令：加速 当前=%lu 目标=%lu", current, target_duty);
        return true;
    });

    // 减速（固定档位降低，用户无需输入）
    mcp_server.AddTool("self.motor.speed_down",
    "Decrease the motor speed by one level.\n"
    "Use this tool for:\n"
    "1. The user uses Chinese/English phrases to request slower motor speed, such as: \n"
    "   - Chinese: 减速、转慢点、转慢一点、慢一点、慢点、转速调低、降低转速、转得慢些、放慢速度\n"
    "   - English: 'slow down', 'slower', 'reduce speed', 'make it gentle', 'less speed', 'decrease rotation speed'\n"
    "2. Gradually make the motor run slower.",
    PropertyList(),
    [this](const PropertyList &properties) -> ReturnValue
    {
        uint32_t current = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        uint32_t step = 1000;      // 每次减速步长
        uint32_t target_duty = current - step;
        
        if (target_duty > current) // 防止下溢
            target_duty = 0;
        
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        working_ = (target_duty > 0);
        
        ESP_LOGI(TAG, "MCP指令：减速 当前=%lu 目标=%lu", current, target_duty);
        return true;
    });
    
}

void motor::motor_test()
{
                            uint32_t target_duty = 8000;
                            // 新增：强制校验，防止极端情况越界
                            ESP_LOGI(TAG, "测试，设置目标占空比%ld", target_duty);
                            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_duty);
                            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                          
}
