import io
p = r"firmware\digicore-c3\main\app_main.cpp"
s = io.open(p, encoding="utf-8").read()
old = """    if (err != ESP_OK) {
        s_have_prev = false;
        s_blit_errs++;"""
new = """    if (err != ESP_OK) {
        s_have_prev = false;
        s_blit_errs++;"""
assert old in s
s = s.replace("""        s_blits++;
        s_blits_total++;
    }
    if (err != ESP_OK) {""", """        s_blits++;
        s_blits_total++;
    }
    if (err == ESP_OK)
        s_have_prev = true;
    if (err != ESP_OK) {""")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
