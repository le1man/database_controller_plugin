$ImageName = "database-controller-x86"
$ContainerName = "temp-build-x86"
$OutputPath = "libdatabase_controller_plugin.so"
$DockerfilePath = "."

Write-Host "build docker-image '$ImageName'..."
docker build -t $ImageName $DockerfilePath
if ($LASTEXITCODE -ne 0){
		Write-Error "build error"
}

Write-Host "Create temporary container '$ContainerName'..."
docker create --name $ContainerName $ImageName | Out-Null
if ($LASTEXITCODE -ne 0){
		Write-Error "create container error"
}

Write-Host "Copy '$OutputPath' from container to host..."
docker cp "${ContainerName}:/opt/project/build/$OutputPath" $OutputPath
if ($LASTEXITCODE -ne 0){
		Write-Error "Error copy file"
		docker rm -f $ContainerName | Out-Null
		exit 1
}

docker rm $ContainerName | Out-Null
