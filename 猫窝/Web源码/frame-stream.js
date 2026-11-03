// A frame is a 4-byte network-order length followed by exactly that many JPEG bytes.
export class FrameDecoder {
  buffer = new Uint8Array(0);
  static MAX_FRAME = 512 * 1024;
  push(chunk) {
    const joined = new Uint8Array(this.buffer.length + chunk.length);
    joined.set(this.buffer); joined.set(chunk, this.buffer.length);
    const frames = [];
    let offset = 0;
    while (joined.length - offset >= 4) {
      const length = new DataView(joined.buffer, joined.byteOffset + offset, 4).getUint32(0);
      if (length < 4 || length > FrameDecoder.MAX_FRAME) throw new Error('摄像头视频帧长度异常');
      if (joined.length - offset < 4 + length) break;
      const jpeg = joined.slice(offset + 4, offset + 4 + length);
      if (jpeg[0] !== 255 || jpeg[1] !== 216 || jpeg.at(-2) !== 255 || jpeg.at(-1) !== 217) throw new Error('摄像头视频帧不完整');
      frames.push(jpeg);
      offset += length + 4;
    }
    this.buffer = joined.slice(offset);
    return frames;
  }
}
