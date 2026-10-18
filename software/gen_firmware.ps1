$path = ".\firmware.bin"   # 输出文件
$size = 256KB              # 指定大小，例如 1MB、10MB

# 创建指定长度的字节数组
$bytes = [byte[]]::new($size)

# 用加密安全随机数填充
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($bytes)

# 写入文件
[System.IO.File]::WriteAllBytes($path, $bytes)
