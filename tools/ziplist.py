
import zipfile
p=r'C:\Users\DuWeisi\mame\roms\digicolr.zip'
z=zipfile.ZipFile(p)
for i in z.infolist():
    print(i.filename, 'size', i.file_size, 'compressed', i.compress_size, 'ratio', round(i.compress_size/i.file_size,4) if i.file_size else 0)
