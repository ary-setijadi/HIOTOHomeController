const amqp = require('amqplib');
(async () => {
  const c = await amqp.connect({hostname:'192.168.137.44', port:5672, vhost:'/', username:'admin', password:'123456Aa!'});
  const ch = await c.createChannel();
  await ch.assertExchange('home.automation', 'topic', {durable:true});
  const msg = JSON.stringify({msg_id:'test-0001', ts:new Date().toISOString(), source:'SNS-SW-0001', message_class:'state', payload:{serial_number:'SNS-SW-0001', digital_value:[1]}});
  ch.publish('home.automation', 'home.sensor.1.SNS-SW-0001.state', Buffer.from(msg), {persistent:true});
  console.log('published:', msg);
  await ch.close(); await c.close();
})().catch(e => { console.error(e.message); process.exit(1); });
