// 头像：有上传的头像则显示图片，否则显示昵称首字
export default function Avatar({ name, url, size }: { name: string; url?: string | null; size?: 'lg' | number }) {
  const style = typeof size === 'number' ? { width: size, height: size } : undefined
  const cls = 'avatar' + (size === 'lg' ? ' lg' : '')
  if (url) return <img className={cls} style={style} src={url} alt="" />
  return (
    <span className={cls} style={style}>
      {name.slice(0, 1)}
    </span>
  )
}
