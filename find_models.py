import requests

# Search for image-to-text models with inference enabled
r = requests.get("https://huggingface.co/api/models", params={
    "pipeline_tag": "image-to-text",
    "sort": "downloads",
    "direction": "-1",
    "limit": 20,
    "inference": "warm"
}, timeout=15)
models = r.json()
print(f"Found {len(models)} image-to-text models with warm inference:\n")
for m in models:
    mid = m.get("modelId", "?")
    dl = m.get("downloads", 0)
    inf = m.get("inference", "?")
    print(f"  {mid:55s}  downloads={dl:>10,}  inference={inf}")
