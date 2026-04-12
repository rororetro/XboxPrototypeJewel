import sys, os, cv2, io
from PIL import Image

def generate_outbuf(framecount, framedata, line_dma_sizes):
    f = io.BytesIO()
    f.write(b'RLEV')
    f.write(framecount.to_bytes(4, 'little'))
    f.write(int(0x14).to_bytes(4, 'little'))

    # get some file offsets so we can fill this in later
    frame_size_offset_offset = f.tell()
    f.seek(4, os.SEEK_CUR)
    framedata_offset_offset = f.tell()
    f.seek(4, os.SEEK_CUR)

    # write out frame size list
    for size in line_dma_sizes:
        f.write(size.to_bytes(2, 'little'))

    # fill in some missing header info, now that we have it.
    frame_size_offset_val = f.tell()
    f.seek(frame_size_offset_offset)
    f.write(frame_size_offset_val.to_bytes(4, 'little'))
    f.seek(frame_size_offset_val)

    # generate frame size list
    for frame in framedata:
        f.write(len(frame).to_bytes(4, 'little'))

    framedata_offset_val = f.tell()
    f.seek(framedata_offset_offset)
    f.write(framedata_offset_val.to_bytes(4, 'little'))
    f.seek(framedata_offset_val)

    # write framedata
    for frame in framedata:
        f.write(frame)

    return f.getvalue()

# this code is terrible
def find_repeats(data, line_offset):
    match_data = bytearray(data[0:4])
    match_start = 0
    is_match = False
    matches = []

    for j in range(4, 800, 4):
        if match_data == data[j:j+4]:
            is_match = True
            continue
        elif is_match:
            is_match = False
            matches.append((line_offset+match_start, line_offset+j, bytearray(match_data)))

        match_start = j
        match_data[0:4] = data[j:j+4]


    if is_match:
        matches.append((line_offset+match_start, line_offset+800, bytearray(match_data)))

    return matches

# this code is also pretty terrible.
def compress_frame(image_data):
    lines = []
    line_sizes = []
    is_match = False
    match_data = bytearray(4)
    match_start = 0

    # get a list of repeats for each line. We can reconstruct any non-repeated data later, using this data
    for i in range(0, len(image_data), 800):
        lines.append(find_repeats(image_data[i:i+800], i))

    out = bytearray()
    previous_end = 0

    for line in lines:
        dma_size = 0
        for item in line:
            # if the previous repeated data's end doesn't line up with the new repeated data's start,
            # there must be some non-repeated data we need to account for.
            if previous_end != item[0]:
                # encode a HSTX_CMD_TMDS command
                out += ((2 << 12) | (item[0] - previous_end)).to_bytes(4, 'little')
                out += image_data[previous_end:item[0]] # non-repeated line data
                dma_size += 4 + (item[0] - previous_end) # command takes 4 bytes plus the size of this data

            # encode a HSTX_CMD_TMDS_REPEAT command
            out += ((3 << 12) | (item[1] - item[0])).to_bytes(4, 'little')
            out += item[2] # repeated line data
            dma_size += 8  # command takes 4 bytes, repeated data takes 4 as well
            previous_end = item[1] # current end becomes the previous

        # there's potential that the end of the frame is composed of non-repeated data, so we account for that here.
        if (previous_end % 800) != 0:
            difference = 800 - (previous_end % 800)
            # encode a HSTX_CMD_TMDS command
            out += ((2 << 12) | difference).to_bytes(4, 'little')
            out += image_data[previous_end:previous_end+difference]
            dma_size += 4 + difference
            previous_end += difference

        # track the size of each compressed line, so that later we know how much to tell the DMA engine to copy.
        line_sizes.append(dma_size)

    return out, line_sizes

def rgb888to332(input):
    output = bytearray(len(input)//3)

    for i in range(0, len(input)//3):
        output[i] = input[i*3+0] & 0xE0 | ((input[i*3+1] & 0xE0) >> 3) | ((input[i*3+2] & 0xC0) >> 6)

    return output

def imageFromOpenCvFrame(frame):
    im = Image.new("RGB", (800, 800))
    pixels = im.load()

    for i, line in enumerate(frame):
        for j, pixel in enumerate(line):
            pixels[j, i] = (pixel[2], pixel[1], pixel[0])

    return im

cap = cv2.VideoCapture(sys.argv[1])
if not cap.isOpened():
    print("Error: Could not open video. Unsupported format?")
    exit()

frames = []

while True:
    ret, frame = cap.read()

    if not ret:
        break

    if len(frame) != 800 or len(frame[0]) != 800:
        print("Video must be 800x800!")
        exit()

    im = imageFromOpenCvFrame(frame)
    frames.append(im)

palette_im = Image.new("RGB", (256, 1))
palette_vals = palette_im.load()

index = 0
for r in range(0, 8):
    for g in range(0, 8):
        for b in range(0, 4):
            palette_vals[index, 0] = (r << 5, g << 5, b << 6)
            index += 1


framedata_sizes = []
line_sizes_out = []
framedata_out = []

for i, frame in enumerate(frames):
    im = frame.quantize(colors=256, palette=palette_im.getpalette(), dither=Image.FLOYDSTEINBERG)

    rgb888 = im.convert("RGB").tobytes()
    rgb332 = rgb888to332(rgb888)

    frame, line_sizes = compress_frame(rgb332)

    if len(frame) > 256000:
        print(f"frame {i} length exceeds framebuffer size!")
        exit()

    framedata_sizes.append(len(frame))
    line_sizes_out = line_sizes_out + line_sizes
    framedata_out.append(frame)

out = generate_outbuf(len(frames), framedata_out, line_sizes_out)
out_f = open(f"{sys.argv[1]}.h", 'w')
out_f.write("static const uint8_t __attribute__((aligned(4))) animation[] = {")
for i, byte in enumerate(out):
    if (i % 0x10) == 0:
        out_f.write('\n    ')
    out_f.write(f'0x{byte:02x}, ')
out_f.write("\n};\n")
