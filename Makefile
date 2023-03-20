build:
	docker build . -t nfore/bird

start:
	docker run -d --name bird nfore/bird

stop:
	docker stop bird
	docker rm bird