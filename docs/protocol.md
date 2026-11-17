# QtChat TCP 协议 v1

每帧为4字节无符号大端长度 + UTF-8 JSON；长度不含前缀，范围1–65536。字节向量 `00 00 00 02 7b 7d` 表示 `{}`，业务层会因缺少字段拒绝它。

请求：`{"version":1,"type":"login","request_id":"最多64字节的非空唯一编号","data":{"username":"alice","password":"Secret-123"}}`。

成功：相同 version/type/request_id，`ok:true,data:{...}`。失败：`ok:false,error:{code,message}`。事件没有 request_id，使用 `version:1,event:true,type,data`。客户端不能指定发送者身份；data中的未知字段会被拒绝。

| type | data | 成功 data |
| --- | --- | --- |
| register | username,display_name,password | user；注册不登录 |
| login | username,password | user；后登录替换旧连接 |
| logout | {} | {}；清除连接身份 |
| ping | {} | pong:true；允许未登录发送 |
| user_search | username，精确匹配 | user,is_friend |
| friend_list | {} | friends:[user]（网络层补online）,requests:[{id,from:user,created_at}] |
| friend_request | peer_id | request_id,outgoing；重复/交叉待处理申请返回同一编号 |
| friend_respond | request_id,accept:boolean | status:accepted/rejected；仅接收者可处理 |
| friend_remove | peer_id | removed:boolean；双向解除，保留历史 |
| message_send | peer_id,client_id:UUID,text | message,duplicate；入库提交后回复 |
| history | peer_id,before_id?,after_id?,limit? | messages（升序）,has_more,peer,is_friend |
| conversations | before_id?,limit? | conversations:[{peer,last_message,unread,is_friend}],has_more,next_before_id |
| read | peer_id,up_to_id | updated；仅标记发给当前用户且ID不超过上限的消息 |

user仅含id、username、display_name。消息含id、sender_id、receiver_id、client_id、text、created_at（UTC ISO格式）、is_read。正文1–8192 UTF-8字节。client_id按发送者去重；同编号不同接收者或文本返回CONFLICT；重复相同消息返回原记录且不重复推送。确认表示服务器已保存，不表示对方已读。

history默认50条、最多100条，同时受56000字节记录预算限制。before_id向前翻页，after_id用于重连补齐；两个游标字段不能同时指定。before返回临近游标之前一页，after返回之后最早一页；显式after_id=0从最早消息开始。has_more为真时继续用当前响应页的边界ID请求。双方客户端按服务端消息ID去重，实时推送和发送确认不会推进历史同步游标。

conversations默认50项、最多100项，同时受56000字节项目预算限制，按末条消息ID降序返回。has_more为真时将next_before_id作为下一页before_id；before_id=0表示最新页。摘要中的last_message.text最多256个UTF-8字节，保持字符完整，text_truncated标明是否截断；完整正文通过history取得。客户端自动读取后续摘要页。

事件：friend_request的data为id/from；friends_changed为peer_id；message为message；presence为user_id/online；session_replaced为message。在线状态只向好友推送，由服务器当前在线映射确定。

错误码：INVALID_REQUEST、UNAUTHENTICATED、AUTH_FAILED、USERNAME_TAKEN、NOT_FOUND、FORBIDDEN、ALREADY_FRIENDS、NOT_FRIENDS、CONFLICT、SERVER_ERROR；网络层另有BUSY、RESPONSE_TOO_LARGE。客户端本地错误使用OFFLINE、TIMEOUT、DISCONNECTED、INVALID_RESPONSE。

权限检查以认证连接身份为准。好友关系检查与消息写入位于同一SQLite事务。密码不进入日志、数据库明文或客户端配置。协议为普通TCP，默认监听回环地址。
