import io
s = io.open("firmware/digicore-c3/main/display.c", encoding="utf-8").read()
i = s.find("esp_lcd_panel_swap_xy(s_panel, true)")
print(s[max(0,i-700):i+320])
