static mut GLOBAL: u32 = 0;

async fn increment1(i: u32) {
    tokio::time::sleep(std::time::Duration::from_millis(1)).await;
    unsafe {
        GLOBAL += i;
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    // measure the time it takes to spawn 10,000 tasks
    {
        println!("rust coro");

        let ts = std::time::Instant::now();

        let mut tasks = Vec::new();
        for i in 0..10_000 {
            tasks.push(tokio::spawn(async move {
                increment1(i).await;
            }));
        }
        futures::future::join_all(tasks).await;
        println!("result: {}", unsafe { GLOBAL });

        println!("{}ms", ts.elapsed().as_millis());
    }

    // measure the time it takes to spawn 10,000 tasks and not await
    {
        println!("rust coro no await");

        let ts = std::time::Instant::now();

        for i in 0..10_000 {
            tokio::spawn(async move {
                increment1(i).await;
            });
        }
        println!("result: {}", unsafe { GLOBAL });
        println!("{}ms", ts.elapsed().as_millis());
    }
}
